// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 */

#include <linux/bio.h>
#include <linux/bitmap.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/pagemap.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/zstd.h>
#include "misc.h"
#include "fs.h"
#include "btrfs_inode.h"
#include "compression.h"
#include "super.h"

#define ZSTD_BTRFS_MAX_WINDOWLOG 17
#define ZSTD_BTRFS_MAX_INPUT (1U << ZSTD_BTRFS_MAX_WINDOWLOG)
#define ZSTD_BTRFS_DEFAULT_LEVEL 3
#define ZSTD_BTRFS_MIN_LEVEL -15
#define ZSTD_BTRFS_MAX_LEVEL 15
/* 307s to avoid pathologically clashing with transaction commit */
#define ZSTD_BTRFS_RECLAIM_JIFFIES (307 * HZ)

static zstd_parameters zstd_get_btrfs_parameters(int level,
						 size_t src_len)
{
	zstd_parameters params = zstd_get_params(level, src_len);

	if (params.cParams.windowLog > ZSTD_BTRFS_MAX_WINDOWLOG)
		params.cParams.windowLog = ZSTD_BTRFS_MAX_WINDOWLOG;
	WARN_ON(src_len > ZSTD_BTRFS_MAX_INPUT);
	return params;
}

struct workspace {
	void *mem;
	size_t size;
	char *buf;
	int level;
	int req_level;
	unsigned long last_used; /* jiffies */
	struct list_head list;
	struct list_head lru_list;
	zstd_in_buffer in_buf;
	zstd_out_buffer out_buf;
	zstd_parameters params;
};

/*
 * Zstd Workspace Management
 *
 * Zstd workspaces have different memory requirements depending on the level.
 * The zstd workspaces are managed by having individual lists for each level
 * and a global lru.  Forward progress is maintained by protecting a max level
 * workspace.
 *
 * Getting a workspace is done by using the bitmap to identify the levels that
 * have available workspaces and scans up.  This lets us recycle higher level
 * workspaces because of the monotonic memory guarantee.  A workspace's
 * last_used is only updated if it is being used by the corresponding memory
 * level.  Putting a workspace involves adding it back to the appropriate places
 * and adding it back to the lru if necessary.
 *
 * A timer is used to reclaim workspaces if they have not been used for
 * ZSTD_BTRFS_RECLAIM_JIFFIES.  This helps keep only active workspaces around.
 * The upper bound is provided by the workqueue limit which is 2 (percpu limit).
 */

struct zstd_workspace_manager {
	const struct btrfs_compress_op *ops;
	spinlock_t lock;
	struct list_head lru_list;
	struct list_head idle_ws[ZSTD_BTRFS_MAX_LEVEL];
	unsigned long active_map;
	wait_queue_head_t wait;
	struct timer_list timer;
};

static struct zstd_workspace_manager wsm;

static size_t zstd_ws_mem_sizes[ZSTD_BTRFS_MAX_LEVEL];

static inline struct workspace *list_to_workspace(struct list_head *list)
{
	return container_of(list, struct workspace, list);
}

static inline int clip_level(int level)
{
	return max(0, level - 1);
}

/*
 * Timer callback to free unused workspaces.
 *
 * @t: timer
 *
 * This scans the lru_list and attempts to reclaim any workspace that hasn't
 * been used for ZSTD_BTRFS_RECLAIM_JIFFIES.
 *
 * The context is softirq and does not need the _bh locking primitives.
 */
static void zstd_reclaim_timer_fn(struct timer_list *timer)
{
	unsigned long reclaim_threshold = jiffies - ZSTD_BTRFS_RECLAIM_JIFFIES;
	struct list_head *pos, *next;

	ASSERT(timer == &wsm.timer);

	spin_lock(&wsm.lock);

	if (list_empty(&wsm.lru_list)) {
		spin_unlock(&wsm.lock);
		return;
	}

	list_for_each_prev_safe(pos, next, &wsm.lru_list) {
		struct workspace *victim = container_of(pos, struct workspace,
							lru_list);
		int level;

		if (time_after(victim->last_used, reclaim_threshold))
			break;

		/* workspace is in use */
		if (victim->req_level)
			continue;

		level = victim->level;
		list_del(&victim->lru_list);
		list_del(&victim->list);
		zstd_free_workspace(&victim->list);

		if (list_empty(&wsm.idle_ws[level]))
			clear_bit(level, &wsm.active_map);

	}

	if (!list_empty(&wsm.lru_list))
		mod_timer(&wsm.timer, jiffies + ZSTD_BTRFS_RECLAIM_JIFFIES);

	spin_unlock(&wsm.lock);
}

/*
 * Calculate monotonic memory bounds.
 *
 * It is possible based on the level configurations that a higher level
 * workspace uses less memory than a lower level workspace.  In order to reuse
 * workspaces, this must be made a monotonic relationship.  This precomputes
 * the required memory for each level and enforces the monotonicity between
 * level and memory required.
 */
static void zstd_calc_ws_mem_sizes(void)
{
	size_t max_size = 0;
	int level;

	for (level = ZSTD_BTRFS_MIN_LEVEL; level <= ZSTD_BTRFS_MAX_LEVEL; level++) {
		if (level == 0)
			continue;
		zstd_parameters params =
			zstd_get_btrfs_parameters(level, ZSTD_BTRFS_MAX_INPUT);
		size_t level_size =
			max_t(size_t,
			      zstd_cstream_workspace_bound(&params.cParams),
			      zstd_dstream_workspace_bound(ZSTD_BTRFS_MAX_INPUT));

		max_size = max_t(size_t, max_size, level_size);
		/* Use level 1 workspace size for all the fast mode negative levels. */
		zstd_ws_mem_sizes[clip_level(level)] = max_size;
	}
}

void zstd_init_workspace_manager(void)
{
	struct list_head *ws;
	int i;

	zstd_calc_ws_mem_sizes();

	wsm.ops = &btrfs_zstd_compress;
	spin_lock_init(&wsm.lock);
	init_waitqueue_head(&wsm.wait);
	timer_setup(&wsm.timer, zstd_reclaim_timer_fn, 0);

	INIT_LIST_HEAD(&wsm.lru_list);
	for (i = 0; i < ZSTD_BTRFS_MAX_LEVEL; i++)
		INIT_LIST_HEAD(&wsm.idle_ws[i]);

	ws = zstd_alloc_workspace(ZSTD_BTRFS_MAX_LEVEL);
	if (IS_ERR(ws)) {
		pr_warn(
		"BTRFS: cannot preallocate zstd compression workspace\n");
	} else {
		set_bit(ZSTD_BTRFS_MAX_LEVEL - 1, &wsm.active_map);
		list_add(ws, &wsm.idle_ws[ZSTD_BTRFS_MAX_LEVEL - 1]);
	}
}

void zstd_cleanup_workspace_manager(void)
{
	struct workspace *workspace;
	int i;

	spin_lock_bh(&wsm.lock);
	for (i = 0; i < ZSTD_BTRFS_MAX_LEVEL; i++) {
		while (!list_empty(&wsm.idle_ws[i])) {
			workspace = container_of(wsm.idle_ws[i].next,
						 struct workspace, list);
			list_del(&workspace->list);
			list_del(&workspace->lru_list);
			zstd_free_workspace(&workspace->list);
		}
	}
	spin_unlock_bh(&wsm.lock);

	timer_delete_sync(&wsm.timer);
}

/*
 * Find workspace for given level.
 *
 * @level: compression level
 *
 * This iterates over the set bits in the active_map beginning at the requested
 * compression level.  This lets us utilize already allocated workspaces before
 * allocating a new one.  If the workspace is of a larger size, it is used, but
 * the place in the lru_list and last_used times are not updated.  This is to
 * offer the opportunity to reclaim the workspace in favor of allocating an
 * appropriately sized one in the future.
 */
static struct list_head *zstd_find_workspace(int level)
{
	struct list_head *ws;
	struct workspace *workspace;
	int i = clip_level(level);

	spin_lock_bh(&wsm.lock);
	for_each_set_bit_from(i, &wsm.active_map, ZSTD_BTRFS_MAX_LEVEL) {
		if (!list_empty(&wsm.idle_ws[i])) {
			ws = wsm.idle_ws[i].next;
			workspace = list_to_workspace(ws);
			list_del_init(ws);
			/* keep its place if it's a lower level using this */
			workspace->req_level = level;
			if (clip_level(level) == workspace->level)
				list_del(&workspace->lru_list);
			if (list_empty(&wsm.idle_ws[i]))
				clear_bit(i, &wsm.active_map);
			spin_unlock_bh(&wsm.lock);
			return ws;
		}
	}
	spin_unlock_bh(&wsm.lock);

	return NULL;
}

/*
 * Zstd get_workspace for level.
 *
 * @level: compression level
 *
 * If @level is 0, then any compression level can be used.  Therefore, we begin
 * scanning from 1.  We first scan through possible workspaces and then after
 * attempt to allocate a new workspace.  If we fail to allocate one due to
 * memory pressure, go to sleep waiting for the max level workspace to free up.
 */
struct list_head *zstd_get_workspace(int level)
{
	struct list_head *ws;
	unsigned int nofs_flag;

	/* level == 0 means we can use any workspace */
	if (!level)
		level = 1;

again:
	ws = zstd_find_workspace(level);
	if (ws)
		return ws;

	nofs_flag = memalloc_nofs_save();
	ws = zstd_alloc_workspace(level);
	memalloc_nofs_restore(nofs_flag);

	if (IS_ERR(ws)) {
		DEFINE_WAIT(wait);

		prepare_to_wait(&wsm.wait, &wait, TASK_UNINTERRUPTIBLE);
		schedule();
		finish_wait(&wsm.wait, &wait);

		goto again;
	}

	return ws;
}

/*
 * Zstd put_workspace.
 *
 * @ws: list_head for the workspace
 *
 * When putting back a workspace, we only need to update the LRU if we are of
 * the requested compression level.  Here is where we continue to protect the
 * max level workspace or update last_used accordingly.  If the reclaim timer
 * isn't set, it is also set here.  Only the max level workspace tries and wakes
 * up waiting workspaces.
 */
void zstd_put_workspace(struct list_head *ws)
{
	struct workspace *workspace = list_to_workspace(ws);

	spin_lock_bh(&wsm.lock);

	/* A node is only taken off the lru if we are the corresponding level */
	if (clip_level(workspace->req_level) == workspace->level) {
		/* Hide a max level workspace from reclaim */
		if (list_empty(&wsm.idle_ws[ZSTD_BTRFS_MAX_LEVEL - 1])) {
			INIT_LIST_HEAD(&workspace->lru_list);
		} else {
			workspace->last_used = jiffies;
			list_add(&workspace->lru_list, &wsm.lru_list);
			if (!timer_pending(&wsm.timer))
				mod_timer(&wsm.timer,
					  jiffies + ZSTD_BTRFS_RECLAIM_JIFFIES);
		}
	}

	set_bit(workspace->level, &wsm.active_map);
	list_add(&workspace->list, &wsm.idle_ws[workspace->level]);
	workspace->req_level = 0;

	spin_unlock_bh(&wsm.lock);

	if (workspace->level == clip_level(ZSTD_BTRFS_MAX_LEVEL))
		cond_wake_up(&wsm.wait);
}

void zstd_free_workspace(struct list_head *ws)
{
	struct workspace *workspace = list_entry(ws, struct workspace, list);

	kvfree(workspace->mem);
	kfree(workspace->buf);
	kfree(workspace);
}

struct list_head *zstd_alloc_workspace(int level)
{
	struct workspace *workspace;

	workspace = kzalloc(sizeof(*workspace), GFP_KERNEL);
	if (!workspace)
		return ERR_PTR(-ENOMEM);

	/* Use level 1 workspace size for all the fast mode negative levels. */
	workspace->size = zstd_ws_mem_sizes[clip_level(level)];
	workspace->level = clip_level(level);
	workspace->req_level = level;
	workspace->last_used = jiffies;
	workspace->mem = kvmalloc(workspace->size, GFP_KERNEL | __GFP_NOWARN);
	workspace->buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!workspace->mem || !workspace->buf)
		goto fail;

	INIT_LIST_HEAD(&workspace->list);
	INIT_LIST_HEAD(&workspace->lru_list);

	return &workspace->list;
fail:
	zstd_free_workspace(&workspace->list);
	return ERR_PTR(-ENOMEM);
}

int zstd_compress_folios(struct list_head *ws, struct address_space *mapping,
			 u64 start, struct folio **folios, unsigned long *out_folios,
			 unsigned long *total_in, unsigned long *total_out)
{
	struct workspace *workspace = list_entry(ws, struct workspace, list);
	zstd_cstream *stream;
	int ret = 0;
	int nr_folios = 0;
	struct folio *in_folio = NULL;  /* The current folio to read. */
	struct folio *out_folio = NULL; /* The current folio to write to. */
	unsigned long tot_in = 0;
	unsigned long tot_out = 0;
	unsigned long len = *total_out;
	const unsigned long nr_dest_folios = *out_folios;
	const u64 orig_end = start + len;
	unsigned long max_out = nr_dest_folios * PAGE_SIZE;
	unsigned int cur_len;

	workspace->params = zstd_get_btrfs_parameters(workspace->req_level, len);
	*out_folios = 0;
	*total_out = 0;
	*total_in = 0;

	/* Initialize the stream */
	stream = zstd_init_cstream(&workspace->params, len, workspace->mem,
			workspace->size);
	if (unlikely(!stream)) {
		struct btrfs_inode *inode = BTRFS_I(mapping->host);

		btrfs_err(inode->root->fs_info,
	"zstd compression init level %d failed, root %llu inode %llu offset %llu",
			  workspace->req_level, btrfs_root_id(inode->root),
			  btrfs_ino(inode), start);
		ret = -EIO;
		goto out;
	}

	/* map in the first page of input data */
	ret = btrfs_compress_filemap_get_folio(mapping, start, &in_folio);
	if (ret < 0)
		goto out;
	cur_len = btrfs_calc_input_length(orig_end, start);
	workspace->in_buf.src = kmap_local_folio(in_folio, offset_in_page(start));
	workspace->in_buf.pos = 0;
	workspace->in_buf.size = cur_len;

	/* Allocate and map in the output buffer */
	out_folio = btrfs_alloc_compr_folio();
	if (out_folio == NULL) {
		ret = -ENOMEM;
		goto out;
	}
	folios[nr_folios++] = out_folio;
	workspace->out_buf.dst = folio_address(out_folio);
	workspace->out_buf.pos = 0;
	workspace->out_buf.size = min_t(size_t, max_out, PAGE_SIZE);

	while (1) {
		size_t ret2;

		ret2 = zstd_compress_stream(stream, &workspace->out_buf,
				&workspace->in_buf);
		if (unlikely(zstd_is_error(ret2))) {
			struct btrfs_inode *inode = BTRFS_I(mapping->host);

			btrfs_warn(inode->root->fs_info,
"zstd compression level %d failed, error %d root %llu inode %llu offset %llu",
				   workspace->req_level, zstd_get_error_code(ret2),
				   btrfs_root_id(inode->root), btrfs_ino(inode),
				   start);
			ret = -EIO;
			goto out;
		}

		/* Check to see if we are making it bigger */
		if (tot_in + workspace->in_buf.pos > 8192 &&
				tot_in + workspace->in_buf.pos <
				tot_out + workspace->out_buf.pos) {
			ret = -E2BIG;
			goto out;
		}

		/* We've reached the end of our output range */
		if (workspace->out_buf.pos >= max_out) {
			tot_out += workspace->out_buf.pos;
			ret = -E2BIG;
			goto out;
		}

		/* Check if we need more output space */
		if (workspace->out_buf.pos == workspace->out_buf.size) {
			tot_out += PAGE_SIZE;
			max_out -= PAGE_SIZE;
			if (nr_folios == nr_dest_folios) {
				ret = -E2BIG;
				goto out;
			}
			out_folio = btrfs_alloc_compr_folio();
			if (out_folio == NULL) {
				ret = -ENOMEM;
				goto out;
			}
			folios[nr_folios++] = out_folio;
			workspace->out_buf.dst = folio_address(out_folio);
			workspace->out_buf.pos = 0;
			workspace->out_buf.size = min_t(size_t, max_out,
							PAGE_SIZE);
		}

		/* We've reached the end of the input */
		if (workspace->in_buf.pos >= len) {
			tot_in += workspace->in_buf.pos;
			break;
		}

		/* Check if we need more input */
		if (workspace->in_buf.pos == workspace->in_buf.size) {
			tot_in += workspace->in_buf.size;
			kunmap_local(workspace->in_buf.src);
			workspace->in_buf.src = NULL;
			folio_put(in_folio);
			start += cur_len;
			len -= cur_len;
			ret = btrfs_compress_filemap_get_folio(mapping, start, &in_folio);
			if (ret < 0)
				goto out;
			cur_len = btrfs_calc_input_length(orig_end, start);
			workspace->in_buf.src = kmap_local_folio(in_folio,
							 offset_in_page(start));
			workspace->in_buf.pos = 0;
			workspace->in_buf.size = cur_len;
		}
	}
	while (1) {
		size_t ret2;

		ret2 = zstd_end_stream(stream, &workspace->out_buf);
		if (unlikely(zstd_is_error(ret2))) {
			struct btrfs_inode *inode = BTRFS_I(mapping->host);

			btrfs_err(inode->root->fs_info,
"zstd compression end level %d failed, error %d root %llu inode %llu offset %llu",
				  workspace->req_level, zstd_get_error_code(ret2),
				  btrfs_root_id(inode->root), btrfs_ino(inode),
				  start);
			ret = -EIO;
			goto out;
		}
		if (ret2 == 0) {
			tot_out += workspace->out_buf.pos;
			break;
		}
		if (workspace->out_buf.pos >= max_out) {
			tot_out += workspace->out_buf.pos;
			ret = -E2BIG;
			goto out;
		}

		tot_out += PAGE_SIZE;
		max_out -= PAGE_SIZE;
		if (nr_folios == nr_dest_folios) {
			ret = -E2BIG;
			goto out;
		}
		out_folio = btrfs_alloc_compr_folio();
		if (out_folio == NULL) {
			ret = -ENOMEM;
			goto out;
		}
		folios[nr_folios++] = out_folio;
		workspace->out_buf.dst = folio_address(out_folio);
		workspace->out_buf.pos = 0;
		workspace->out_buf.size = min_t(size_t, max_out, PAGE_SIZE);
	}

	if (tot_out >= tot_in) {
		ret = -E2BIG;
		goto out;
	}

	ret = 0;
	*total_in = tot_in;
	*total_out = tot_out;
out:
	*out_folios = nr_folios;
	if (workspace->in_buf.src) {
		kunmap_local(workspace->in_buf.src);
		folio_put(in_folio);
	}
	return ret;
}

int zstd_decompress_bio(struct list_head *ws, struct compressed_bio *cb)
{
	struct workspace *workspace = list_entry(ws, struct workspace, list);
	struct folio **folios_in = cb->compressed_folios;
	size_t srclen = cb->compressed_len;
	zstd_dstream *stream;
	int ret = 0;
	unsigned long folio_in_index = 0;
	unsigned long total_folios_in = DIV_ROUND_UP(srclen, PAGE_SIZE);
	unsigned long buf_start;
	unsigned long total_out = 0;

	stream = zstd_init_dstream(
			ZSTD_BTRFS_MAX_INPUT, workspace->mem, workspace->size);
	if (unlikely(!stream)) {
		struct btrfs_inode *inode = cb->bbio.inode;

		btrfs_err(inode->root->fs_info,
		"zstd decompression init failed, root %llu inode %llu offset %llu",
			  btrfs_root_id(inode->root), btrfs_ino(inode), cb->start);
		ret = -EIO;
		goto done;
	}

	workspace->in_buf.src = kmap_local_folio(folios_in[folio_in_index], 0);
	workspace->in_buf.pos = 0;
	workspace->in_buf.size = min_t(size_t, srclen, PAGE_SIZE);

	workspace->out_buf.dst = workspace->buf;
	workspace->out_buf.pos = 0;
	workspace->out_buf.size = PAGE_SIZE;

	while (1) {
		size_t ret2;

		ret2 = zstd_decompress_stream(stream, &workspace->out_buf,
				&workspace->in_buf);
		if (unlikely(zstd_is_error(ret2))) {
			struct btrfs_inode *inode = cb->bbio.inode;

			btrfs_err(inode->root->fs_info,
		"zstd decompression failed, error %d root %llu inode %llu offset %llu",
				  zstd_get_error_code(ret2), btrfs_root_id(inode->root),
				  btrfs_ino(inode), cb->start);
			ret = -EIO;
			goto done;
		}
		buf_start = total_out;
		total_out += workspace->out_buf.pos;
		workspace->out_buf.pos = 0;

		ret = btrfs_decompress_buf2page(workspace->out_buf.dst,
				total_out - buf_start, cb, buf_start);
		if (ret == 0)
			break;

		if (workspace->in_buf.pos >= srclen)
			break;

		/* Check if we've hit the end of a frame */
		if (ret2 == 0)
			break;

		if (workspace->in_buf.pos == workspace->in_buf.size) {
			kunmap_local(workspace->in_buf.src);
			folio_in_index++;
			if (folio_in_index >= total_folios_in) {
				workspace->in_buf.src = NULL;
				ret = -EIO;
				goto done;
			}
			srclen -= PAGE_SIZE;
			workspace->in_buf.src =
				kmap_local_folio(folios_in[folio_in_index], 0);
			workspace->in_buf.pos = 0;
			workspace->in_buf.size = min_t(size_t, srclen, PAGE_SIZE);
		}
	}
	ret = 0;
done:
	if (workspace->in_buf.src)
		kunmap_local(workspace->in_buf.src);
	return ret;
}

int zstd_decompress(struct list_head *ws, const u8 *data_in,
		struct folio *dest_folio, unsigned long dest_pgoff, size_t srclen,
		size_t destlen)
{
	struct workspace *workspace = list_entry(ws, struct workspace, list);
	struct btrfs_fs_info *fs_info = btrfs_sb(folio_inode(dest_folio)->i_sb);
	const u32 sectorsize = fs_info->sectorsize;
	zstd_dstream *stream;
	int ret = 0;
	unsigned long to_copy = 0;

	stream = zstd_init_dstream(
			ZSTD_BTRFS_MAX_INPUT, workspace->mem, workspace->size);
	if (unlikely(!stream)) {
		struct btrfs_inode *inode = folio_to_inode(dest_folio);

		btrfs_err(inode->root->fs_info,
		"zstd decompression init failed, root %llu inode %llu offset %llu",
			  btrfs_root_id(inode->root), btrfs_ino(inode),
			  folio_pos(dest_folio));
		ret = -EIO;
		goto finish;
	}

	workspace->in_buf.src = data_in;
	workspace->in_buf.pos = 0;
	workspace->in_buf.size = srclen;

	workspace->out_buf.dst = workspace->buf;
	workspace->out_buf.pos = 0;
	workspace->out_buf.size = sectorsize;

	/*
	 * Since both input and output buffers should not exceed one sector,
	 * one call should end the decompression.
	 */
	ret = zstd_decompress_stream(stream, &workspace->out_buf, &workspace->in_buf);
	if (unlikely(zstd_is_error(ret))) {
		struct btrfs_inode *inode = folio_to_inode(dest_folio);

		btrfs_err(inode->root->fs_info,
		"zstd decompression failed, error %d root %llu inode %llu offset %llu",
			  zstd_get_error_code(ret), btrfs_root_id(inode->root),
			  btrfs_ino(inode), folio_pos(dest_folio));
		goto finish;
	}
	to_copy = workspace->out_buf.pos;
	memcpy_to_folio(dest_folio, dest_pgoff, workspace->out_buf.dst, to_copy);
finish:
	/* Error or early end. */
	if (unlikely(to_copy < destlen)) {
		ret = -EIO;
		folio_zero_range(dest_folio, dest_pgoff + to_copy, destlen - to_copy);
	}
	return ret;
}

const struct btrfs_compress_op btrfs_zstd_compress = {
	/* ZSTD uses own workspace manager */
	.workspace_manager = NULL,
	.min_level	= ZSTD_BTRFS_MIN_LEVEL,
	.max_level	= ZSTD_BTRFS_MAX_LEVEL,
	.default_level	= ZSTD_BTRFS_DEFAULT_LEVEL,
};
