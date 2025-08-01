// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Scatterlist Cryptographic API.
 *
 * Copyright (c) 2002 James Morris <jmorris@intercode.com.au>
 * Copyright (c) 2002 David S. Miller (davem@redhat.com)
 * Copyright (c) 2005 Herbert Xu <herbert@gondor.apana.org.au>
 *
 * Portions derived from Cryptoapi, by Alexander Kjeldaas <astor@fast.no>
 * and Nettle, by Niels Möller.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/module.h>
#include <linux/param.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/completion.h>
#include "internal.h"

LIST_HEAD(crypto_alg_list);
EXPORT_SYMBOL_GPL(crypto_alg_list);
DECLARE_RWSEM(crypto_alg_sem);
EXPORT_SYMBOL_GPL(crypto_alg_sem);

BLOCKING_NOTIFIER_HEAD(crypto_chain);
EXPORT_SYMBOL_GPL(crypto_chain);

#if IS_BUILTIN(CONFIG_CRYPTO_ALGAPI) && \
    !IS_ENABLED(CONFIG_CRYPTO_MANAGER_DISABLE_TESTS)
DEFINE_STATIC_KEY_FALSE(__crypto_boot_test_finished);
#endif

static struct crypto_alg *crypto_larval_wait(struct crypto_alg *alg,
					     u32 type, u32 mask);
static struct crypto_alg *crypto_alg_lookup(const char *name, u32 type,
					    u32 mask);

struct crypto_alg *crypto_mod_get(struct crypto_alg *alg)
{
	return try_module_get(alg->cra_module) ? crypto_alg_get(alg) : NULL;
}
EXPORT_SYMBOL_GPL(crypto_mod_get);

void crypto_mod_put(struct crypto_alg *alg)
{
	struct module *module = alg->cra_module;

	crypto_alg_put(alg);
	module_put(module);
}
EXPORT_SYMBOL_GPL(crypto_mod_put);

static struct crypto_alg *__crypto_alg_lookup(const char *name, u32 type,
					      u32 mask)
{
	struct crypto_alg *q, *alg = NULL;
	int best = -2;

	list_for_each_entry(q, &crypto_alg_list, cra_list) {
		int exact, fuzzy;

		if (crypto_is_moribund(q))
			continue;

		if ((q->cra_flags ^ type) & mask)
			continue;

		exact = !strcmp(q->cra_driver_name, name);
		fuzzy = !strcmp(q->cra_name, name);
		if (!exact && !(fuzzy && q->cra_priority > best))
			continue;

		if (unlikely(!crypto_mod_get(q)))
			continue;

		best = q->cra_priority;
		if (alg)
			crypto_mod_put(alg);
		alg = q;

		if (exact)
			break;
	}

	return alg;
}

static void crypto_larval_destroy(struct crypto_alg *alg)
{
	struct crypto_larval *larval = (void *)alg;

	BUG_ON(!crypto_is_larval(alg));
	if (!IS_ERR_OR_NULL(larval->adult))
		crypto_mod_put(larval->adult);
	kfree(larval);
}

struct crypto_larval *crypto_larval_alloc(const char *name, u32 type, u32 mask)
{
	struct crypto_larval *larval;

	larval = kzalloc(sizeof(*larval), GFP_KERNEL);
	if (!larval)
		return ERR_PTR(-ENOMEM);

	type &= ~CRYPTO_ALG_TYPE_MASK | (mask ?: CRYPTO_ALG_TYPE_MASK);

	larval->mask = mask;
	larval->alg.cra_flags = CRYPTO_ALG_LARVAL | type;
	larval->alg.cra_priority = -1;
	larval->alg.cra_destroy = crypto_larval_destroy;

	strscpy(larval->alg.cra_name, name, CRYPTO_MAX_ALG_NAME);
	init_completion(&larval->completion);

	return larval;
}
EXPORT_SYMBOL_GPL(crypto_larval_alloc);

static struct crypto_alg *crypto_larval_add(const char *name, u32 type,
					    u32 mask)
{
	struct crypto_alg *alg;
	struct crypto_larval *larval;

	larval = crypto_larval_alloc(name, type, mask);
	if (IS_ERR(larval))
		return ERR_CAST(larval);

	refcount_set(&larval->alg.cra_refcnt, 2);

	down_write(&crypto_alg_sem);
	alg = __crypto_alg_lookup(name, type, mask);
	if (!alg) {
		alg = &larval->alg;
		list_add(&alg->cra_list, &crypto_alg_list);
	}
	up_write(&crypto_alg_sem);

	if (alg != &larval->alg) {
		kfree(larval);
		if (crypto_is_larval(alg))
			alg = crypto_larval_wait(alg, type, mask);
	}

	return alg;
}

static void crypto_larval_kill(struct crypto_larval *larval)
{
	bool unlinked;

	down_write(&crypto_alg_sem);
	unlinked = list_empty(&larval->alg.cra_list);
	if (!unlinked)
		list_del_init(&larval->alg.cra_list);
	up_write(&crypto_alg_sem);

	if (unlinked)
		return;

	complete_all(&larval->completion);
	crypto_alg_put(&larval->alg);
}

void crypto_schedule_test(struct crypto_larval *larval)
{
	int err;

	err = crypto_probing_notify(CRYPTO_MSG_ALG_REGISTER, larval->adult);
	WARN_ON_ONCE(err != NOTIFY_STOP);
}
EXPORT_SYMBOL_GPL(crypto_schedule_test);

static void crypto_start_test(struct crypto_larval *larval)
{
	if (!crypto_is_test_larval(larval))
		return;

	if (larval->test_started)
		return;

	down_write(&crypto_alg_sem);
	if (larval->test_started) {
		up_write(&crypto_alg_sem);
		return;
	}

	larval->test_started = true;
	up_write(&crypto_alg_sem);

	crypto_schedule_test(larval);
}

static struct crypto_alg *crypto_larval_wait(struct crypto_alg *alg,
					     u32 type, u32 mask)
{
	struct crypto_larval *larval;
	long time_left;

again:
	larval = container_of(alg, struct crypto_larval, alg);

	if (!crypto_boot_test_finished())
		crypto_start_test(larval);

	time_left = wait_for_completion_killable_timeout(
		&larval->completion, 60 * HZ);

	alg = larval->adult;
	if (time_left < 0)
		alg = ERR_PTR(-EINTR);
	else if (!time_left) {
		if (crypto_is_test_larval(larval))
			crypto_larval_kill(larval);
		alg = ERR_PTR(-ETIMEDOUT);
	} else if (!alg || PTR_ERR(alg) == -EEXIST) {
		int err = alg ? -EEXIST : -EAGAIN;

		/*
		 * EEXIST is expected because two probes can be scheduled
		 * at the same time with one using alg_name and the other
		 * using driver_name.  Do a re-lookup but do not retry in
		 * case we hit a quirk like gcm_base(ctr(aes),...) which
		 * will never match.
		 */
		alg = &larval->alg;
		alg = crypto_alg_lookup(alg->cra_name, type, mask) ?:
		      ERR_PTR(err);
	} else if (IS_ERR(alg))
		;
	else if (crypto_is_test_larval(larval) &&
		 !(alg->cra_flags & CRYPTO_ALG_TESTED))
		alg = ERR_PTR(-EAGAIN);
	else if (alg->cra_flags & CRYPTO_ALG_FIPS_INTERNAL)
		alg = ERR_PTR(-EAGAIN);
	else if (!crypto_mod_get(alg))
		alg = ERR_PTR(-EAGAIN);
	crypto_mod_put(&larval->alg);

	if (!IS_ERR(alg) && crypto_is_larval(alg))
		goto again;

	return alg;
}

static struct crypto_alg *crypto_alg_lookup(const char *name, u32 type,
					    u32 mask)
{
	const u32 fips = CRYPTO_ALG_FIPS_INTERNAL;
	struct crypto_alg *alg;
	u32 test = 0;

	if (!((type | mask) & CRYPTO_ALG_TESTED))
		test |= CRYPTO_ALG_TESTED;

	down_read(&crypto_alg_sem);
	alg = __crypto_alg_lookup(name, (type | test) & ~fips,
				  (mask | test) & ~fips);
	if (alg) {
		if (((type | mask) ^ fips) & fips)
			mask |= fips;
		mask &= fips;

		if (!crypto_is_larval(alg) &&
		    ((type ^ alg->cra_flags) & mask)) {
			/* Algorithm is disallowed in FIPS mode. */
			crypto_mod_put(alg);
			alg = ERR_PTR(-ENOENT);
		}
	} else if (test) {
		alg = __crypto_alg_lookup(name, type, mask);
		if (alg && !crypto_is_larval(alg)) {
			/* Test failed */
			crypto_mod_put(alg);
			alg = ERR_PTR(-ELIBBAD);
		}
	}
	up_read(&crypto_alg_sem);

	return alg;
}

static struct crypto_alg *crypto_larval_lookup(const char *name, u32 type,
					       u32 mask)
{
	struct crypto_alg *alg;

	if (!name)
		return ERR_PTR(-ENOENT);

	type &= ~(CRYPTO_ALG_LARVAL | CRYPTO_ALG_DEAD);
	mask &= ~(CRYPTO_ALG_LARVAL | CRYPTO_ALG_DEAD);

	alg = crypto_alg_lookup(name, type, mask);
	if (!alg && !(mask & CRYPTO_NOLOAD)) {
		request_module("crypto-%s", name);

		if (!((type ^ CRYPTO_ALG_NEED_FALLBACK) & mask &
		      CRYPTO_ALG_NEED_FALLBACK))
			request_module("crypto-%s-all", name);

		alg = crypto_alg_lookup(name, type, mask);
	}

	if (!IS_ERR_OR_NULL(alg) && crypto_is_larval(alg))
		alg = crypto_larval_wait(alg, type, mask);
	else if (alg)
		;
	else if (!(mask & CRYPTO_ALG_TESTED))
		alg = crypto_larval_add(name, type, mask);
	else
		alg = ERR_PTR(-ENOENT);

	return alg;
}

int crypto_probing_notify(unsigned long val, void *v)
{
	int ok;

	ok = blocking_notifier_call_chain(&crypto_chain, val, v);
	if (ok == NOTIFY_DONE) {
		request_module("cryptomgr");
		ok = blocking_notifier_call_chain(&crypto_chain, val, v);
	}

	return ok;
}
EXPORT_SYMBOL_GPL(crypto_probing_notify);

struct crypto_alg *crypto_alg_mod_lookup(const char *name, u32 type, u32 mask)
{
	struct crypto_alg *alg;
	struct crypto_alg *larval;
	int ok;

	/*
	 * If the internal flag is set for a cipher, require a caller to
	 * invoke the cipher with the internal flag to use that cipher.
	 * Also, if a caller wants to allocate a cipher that may or may
	 * not be an internal cipher, use type | CRYPTO_ALG_INTERNAL and
	 * !(mask & CRYPTO_ALG_INTERNAL).
	 */
	if (!((type | mask) & CRYPTO_ALG_INTERNAL))
		mask |= CRYPTO_ALG_INTERNAL;

	larval = crypto_larval_lookup(name, type, mask);
	if (IS_ERR(larval) || !crypto_is_larval(larval))
		return larval;

	ok = crypto_probing_notify(CRYPTO_MSG_ALG_REQUEST, larval);

	if (ok == NOTIFY_STOP)
		alg = crypto_larval_wait(larval, type, mask);
	else {
		crypto_mod_put(larval);
		alg = ERR_PTR(-ENOENT);
	}
	crypto_larval_kill(container_of(larval, struct crypto_larval, alg));
	return alg;
}
EXPORT_SYMBOL_GPL(crypto_alg_mod_lookup);

static void crypto_exit_ops(struct crypto_tfm *tfm)
{
	const struct crypto_type *type = tfm->__crt_alg->cra_type;

	if (type && tfm->exit)
		tfm->exit(tfm);
}

static unsigned int crypto_ctxsize(struct crypto_alg *alg, u32 type, u32 mask)
{
	const struct crypto_type *type_obj = alg->cra_type;
	unsigned int len;

	len = alg->cra_alignmask & ~(crypto_tfm_ctx_alignment() - 1);
	if (type_obj)
		return len + type_obj->ctxsize(alg, type, mask);

	switch (alg->cra_flags & CRYPTO_ALG_TYPE_MASK) {
	default:
		BUG();

	case CRYPTO_ALG_TYPE_CIPHER:
		len += crypto_cipher_ctxsize(alg);
		break;
	}

	return len;
}

void crypto_shoot_alg(struct crypto_alg *alg)
{
	down_write(&crypto_alg_sem);
	alg->cra_flags |= CRYPTO_ALG_DYING;
	up_write(&crypto_alg_sem);
}
EXPORT_SYMBOL_GPL(crypto_shoot_alg);

struct crypto_tfm *__crypto_alloc_tfmgfp(struct crypto_alg *alg, u32 type,
					 u32 mask, gfp_t gfp)
{
	struct crypto_tfm *tfm;
	unsigned int tfm_size;
	int err = -ENOMEM;

	tfm_size = sizeof(*tfm) + crypto_ctxsize(alg, type, mask);
	tfm = kzalloc(tfm_size, gfp);
	if (tfm == NULL)
		goto out_err;

	tfm->__crt_alg = alg;
	refcount_set(&tfm->refcnt, 1);

	if (!tfm->exit && alg->cra_init && (err = alg->cra_init(tfm)))
		goto cra_init_failed;

	goto out;

cra_init_failed:
	crypto_exit_ops(tfm);
	if (err == -EAGAIN)
		crypto_shoot_alg(alg);
	kfree(tfm);
out_err:
	tfm = ERR_PTR(err);
out:
	return tfm;
}
EXPORT_SYMBOL_GPL(__crypto_alloc_tfmgfp);

struct crypto_tfm *__crypto_alloc_tfm(struct crypto_alg *alg, u32 type,
				      u32 mask)
{
	return __crypto_alloc_tfmgfp(alg, type, mask, GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(__crypto_alloc_tfm);

/*
 *	crypto_alloc_base - Locate algorithm and allocate transform
 *	@alg_name: Name of algorithm
 *	@type: Type of algorithm
 *	@mask: Mask for type comparison
 *
 *	This function should not be used by new algorithm types.
 *	Please use crypto_alloc_tfm instead.
 *
 *	crypto_alloc_base() will first attempt to locate an already loaded
 *	algorithm.  If that fails and the kernel supports dynamically loadable
 *	modules, it will then attempt to load a module of the same name or
 *	alias.  If that fails it will send a query to any loaded crypto manager
 *	to construct an algorithm on the fly.  A refcount is grabbed on the
 *	algorithm which is then associated with the new transform.
 *
 *	The returned transform is of a non-determinate type.  Most people
 *	should use one of the more specific allocation functions such as
 *	crypto_alloc_skcipher().
 *
 *	In case of error the return value is an error pointer.
 */
struct crypto_tfm *crypto_alloc_base(const char *alg_name, u32 type, u32 mask)
{
	struct crypto_tfm *tfm;
	int err;

	for (;;) {
		struct crypto_alg *alg;

		alg = crypto_alg_mod_lookup(alg_name, type, mask);
		if (IS_ERR(alg)) {
			err = PTR_ERR(alg);
			goto err;
		}

		tfm = __crypto_alloc_tfm(alg, type, mask);
		if (!IS_ERR(tfm))
			return tfm;

		crypto_mod_put(alg);
		err = PTR_ERR(tfm);

err:
		if (err != -EAGAIN)
			break;
		if (fatal_signal_pending(current)) {
			err = -EINTR;
			break;
		}
	}

	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(crypto_alloc_base);

static void *crypto_alloc_tfmmem(struct crypto_alg *alg,
				 const struct crypto_type *frontend, int node,
				 gfp_t gfp)
{
	struct crypto_tfm *tfm;
	unsigned int tfmsize;
	unsigned int total;
	char *mem;

	tfmsize = frontend->tfmsize;
	total = tfmsize + sizeof(*tfm) + frontend->extsize(alg);

	mem = kzalloc_node(total, gfp, node);
	if (mem == NULL)
		return ERR_PTR(-ENOMEM);

	tfm = (struct crypto_tfm *)(mem + tfmsize);
	tfm->__crt_alg = alg;
	tfm->node = node;
	refcount_set(&tfm->refcnt, 1);

	return mem;
}

void *crypto_create_tfm_node(struct crypto_alg *alg,
			     const struct crypto_type *frontend,
			     int node)
{
	struct crypto_tfm *tfm;
	char *mem;
	int err;

	mem = crypto_alloc_tfmmem(alg, frontend, node, GFP_KERNEL);
	if (IS_ERR(mem))
		goto out;

	tfm = (struct crypto_tfm *)(mem + frontend->tfmsize);

	err = frontend->init_tfm(tfm);
	if (err)
		goto out_free_tfm;

	if (!tfm->exit && alg->cra_init && (err = alg->cra_init(tfm)))
		goto cra_init_failed;

	goto out;

cra_init_failed:
	crypto_exit_ops(tfm);
out_free_tfm:
	if (err == -EAGAIN)
		crypto_shoot_alg(alg);
	kfree(mem);
	mem = ERR_PTR(err);
out:
	return mem;
}
EXPORT_SYMBOL_GPL(crypto_create_tfm_node);

void *crypto_clone_tfm(const struct crypto_type *frontend,
		       struct crypto_tfm *otfm)
{
	struct crypto_alg *alg = otfm->__crt_alg;
	struct crypto_tfm *tfm;
	char *mem;

	mem = ERR_PTR(-ESTALE);
	if (unlikely(!crypto_mod_get(alg)))
		goto out;

	mem = crypto_alloc_tfmmem(alg, frontend, otfm->node, GFP_ATOMIC);
	if (IS_ERR(mem)) {
		crypto_mod_put(alg);
		goto out;
	}

	tfm = (struct crypto_tfm *)(mem + frontend->tfmsize);
	tfm->crt_flags = otfm->crt_flags;
	tfm->exit = otfm->exit;

out:
	return mem;
}
EXPORT_SYMBOL_GPL(crypto_clone_tfm);

struct crypto_alg *crypto_find_alg(const char *alg_name,
				   const struct crypto_type *frontend,
				   u32 type, u32 mask)
{
	if (frontend) {
		type &= frontend->maskclear;
		mask &= frontend->maskclear;
		type |= frontend->type;
		mask |= frontend->maskset;
	}

	return crypto_alg_mod_lookup(alg_name, type, mask);
}
EXPORT_SYMBOL_GPL(crypto_find_alg);

/*
 *	crypto_alloc_tfm_node - Locate algorithm and allocate transform
 *	@alg_name: Name of algorithm
 *	@frontend: Frontend algorithm type
 *	@type: Type of algorithm
 *	@mask: Mask for type comparison
 *	@node: NUMA node in which users desire to put requests, if node is
 *		NUMA_NO_NODE, it means users have no special requirement.
 *
 *	crypto_alloc_tfm() will first attempt to locate an already loaded
 *	algorithm.  If that fails and the kernel supports dynamically loadable
 *	modules, it will then attempt to load a module of the same name or
 *	alias.  If that fails it will send a query to any loaded crypto manager
 *	to construct an algorithm on the fly.  A refcount is grabbed on the
 *	algorithm which is then associated with the new transform.
 *
 *	The returned transform is of a non-determinate type.  Most people
 *	should use one of the more specific allocation functions such as
 *	crypto_alloc_skcipher().
 *
 *	In case of error the return value is an error pointer.
 */

void *crypto_alloc_tfm_node(const char *alg_name,
		       const struct crypto_type *frontend, u32 type, u32 mask,
		       int node)
{
	void *tfm;
	int err;

	for (;;) {
		struct crypto_alg *alg;

		alg = crypto_find_alg(alg_name, frontend, type, mask);
		if (IS_ERR(alg)) {
			err = PTR_ERR(alg);
			goto err;
		}

		tfm = crypto_create_tfm_node(alg, frontend, node);
		if (!IS_ERR(tfm))
			return tfm;

		crypto_mod_put(alg);
		err = PTR_ERR(tfm);

err:
		if (err != -EAGAIN)
			break;
		if (fatal_signal_pending(current)) {
			err = -EINTR;
			break;
		}
	}

	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(crypto_alloc_tfm_node);

/*
 *	crypto_destroy_tfm - Free crypto transform
 *	@mem: Start of tfm slab
 *	@tfm: Transform to free
 *
 *	This function frees up the transform and any associated resources,
 *	then drops the refcount on the associated algorithm.
 */
void crypto_destroy_tfm(void *mem, struct crypto_tfm *tfm)
{
	struct crypto_alg *alg;

	if (IS_ERR_OR_NULL(mem))
		return;

	if (!refcount_dec_and_test(&tfm->refcnt))
		return;
	alg = tfm->__crt_alg;

	if (!tfm->exit && alg->cra_exit)
		alg->cra_exit(tfm);
	crypto_exit_ops(tfm);
	crypto_mod_put(alg);
	kfree_sensitive(mem);
}
EXPORT_SYMBOL_GPL(crypto_destroy_tfm);

int crypto_has_alg(const char *name, u32 type, u32 mask)
{
	int ret = 0;
	struct crypto_alg *alg = crypto_alg_mod_lookup(name, type, mask);

	if (!IS_ERR(alg)) {
		crypto_mod_put(alg);
		ret = 1;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(crypto_has_alg);

void crypto_req_done(void *data, int err)
{
	struct crypto_wait *wait = data;

	if (err == -EINPROGRESS)
		return;

	wait->err = err;
	complete(&wait->completion);
}
EXPORT_SYMBOL_GPL(crypto_req_done);

void crypto_destroy_alg(struct crypto_alg *alg)
{
	if (alg->cra_type && alg->cra_type->destroy)
		alg->cra_type->destroy(alg);

	if (alg->cra_destroy)
		alg->cra_destroy(alg);
}
EXPORT_SYMBOL_GPL(crypto_destroy_alg);

MODULE_DESCRIPTION("Cryptographic core API");
MODULE_LICENSE("GPL");
