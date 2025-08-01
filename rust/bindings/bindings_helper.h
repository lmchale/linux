/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Header that contains the code (mostly headers) for which Rust bindings
 * will be automatically generated by `bindgen`.
 *
 * Sorted alphabetically.
 */

#include <kunit/test.h>
#include <linux/blk-mq.h>
#include <linux/blk_types.h>
#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/cred.h>
#include <linux/device/faux.h>
#include <linux/dma-mapping.h>
#include <linux/errname.h>
#include <linux/ethtool.h>
#include <linux/file.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/jump_label.h>
#include <linux/mdio.h>
#include <linux/miscdevice.h>
#include <linux/of_device.h>
#include <linux/pci.h>
#include <linux/phy.h>
#include <linux/pid_namespace.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/tracepoint.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <trace/events/rust_sample.h>

#if defined(CONFIG_DRM_PANIC_SCREEN_QR_CODE)
// Used by `#[export]` in `drivers/gpu/drm/drm_panic_qr.rs`.
#include <drm/drm_panic.h>
#endif

/* `bindgen` gets confused at certain things. */
const size_t RUST_CONST_HELPER_ARCH_SLAB_MINALIGN = ARCH_SLAB_MINALIGN;
const size_t RUST_CONST_HELPER_PAGE_SIZE = PAGE_SIZE;
const gfp_t RUST_CONST_HELPER_GFP_ATOMIC = GFP_ATOMIC;
const gfp_t RUST_CONST_HELPER_GFP_KERNEL = GFP_KERNEL;
const gfp_t RUST_CONST_HELPER_GFP_KERNEL_ACCOUNT = GFP_KERNEL_ACCOUNT;
const gfp_t RUST_CONST_HELPER_GFP_NOWAIT = GFP_NOWAIT;
const gfp_t RUST_CONST_HELPER___GFP_ZERO = __GFP_ZERO;
const gfp_t RUST_CONST_HELPER___GFP_HIGHMEM = ___GFP_HIGHMEM;
const gfp_t RUST_CONST_HELPER___GFP_NOWARN = ___GFP_NOWARN;
const blk_features_t RUST_CONST_HELPER_BLK_FEAT_ROTATIONAL = BLK_FEAT_ROTATIONAL;
