// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/prctl.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/idle.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/pm.h>
#include <linux/tick.h>
#include <linux/random.h>
#include <linux/user-return-notifier.h>
#include <linux/dmi.h>
#include <linux/utsname.h>
#include <linux/stackprotector.h>
#include <linux/cpuidle.h>
#include <linux/acpi.h>
#include <linux/elf-randomize.h>
#include <linux/static_call.h>
#include <trace/events/power.h>
#include <linux/hw_breakpoint.h>
#include <linux/entry-common.h>
#include <asm/cpu.h>
#include <asm/cpuid.h>
#include <asm/apic.h>
#include <linux/uaccess.h>
#include <asm/mwait.h>
#include <asm/fpu/api.h>
#include <asm/fpu/sched.h>
#include <asm/fpu/xstate.h>
#include <asm/debugreg.h>
#include <asm/nmi.h>
#include <asm/tlbflush.h>
#include <asm/mce.h>
#include <asm/vm86.h>
#include <asm/switch_to.h>
#include <asm/desc.h>
#include <asm/prctl.h>
#include <asm/spec-ctrl.h>
#include <asm/io_bitmap.h>
#include <asm/proto.h>
#include <asm/frame.h>
#include <asm/unwind.h>
#include <asm/tdx.h>
#include <asm/mmu_context.h>
#include <asm/shstk.h>

#include "process.h"

/*
 * per-CPU TSS segments. Threads are completely 'soft' on Linux,
 * no more per-task TSS's. The TSS size is kept cacheline-aligned
 * so they are allowed to end up in the .data..cacheline_aligned
 * section. Since TSS's are completely CPU-local, we want them
 * on exact cacheline boundaries, to eliminate cacheline ping-pong.
 */
__visible DEFINE_PER_CPU_PAGE_ALIGNED(struct tss_struct, cpu_tss_rw) = {
	.x86_tss = {
		/*
		 * .sp0 is only used when entering ring 0 from a lower
		 * privilege level.  Since the init task never runs anything
		 * but ring 0 code, there is no need for a valid value here.
		 * Poison it.
		 */
		.sp0 = (1UL << (BITS_PER_LONG-1)) + 1,

#ifdef CONFIG_X86_32
		.sp1 = TOP_OF_INIT_STACK,

		.ss0 = __KERNEL_DS,
		.ss1 = __KERNEL_CS,
#endif
		.io_bitmap_base	= IO_BITMAP_OFFSET_INVALID,
	 },
};
EXPORT_PER_CPU_SYMBOL(cpu_tss_rw);

DEFINE_PER_CPU(bool, __tss_limit_invalid);
EXPORT_PER_CPU_SYMBOL_GPL(__tss_limit_invalid);

/*
 * this gets called so that we can store lazy state into memory and copy the
 * current task into the new thread.
 */
int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src)
{
	/* init_task is not dynamically sized (incomplete FPU state) */
	if (unlikely(src == &init_task))
		memcpy_and_pad(dst, arch_task_struct_size, src, sizeof(init_task), 0);
	else
		memcpy(dst, src, arch_task_struct_size);

#ifdef CONFIG_VM86
	dst->thread.vm86 = NULL;
#endif
	/* Drop the copied pointer to current's fpstate */
	dst->thread.fpu.fpstate = NULL;

	return 0;
}

#ifdef CONFIG_X86_64
void arch_release_task_struct(struct task_struct *tsk)
{
	if (fpu_state_size_dynamic())
		fpstate_free(&tsk->thread.fpu);
}
#endif

/*
 * Free thread data structures etc..
 */
void exit_thread(struct task_struct *tsk)
{
	struct thread_struct *t = &tsk->thread;
	struct fpu *fpu = &t->fpu;

	if (test_thread_flag(TIF_IO_BITMAP))
		io_bitmap_exit(tsk);

	free_vm86(t);

	shstk_free(tsk);
	fpu__drop(fpu);
}

static int set_new_tls(struct task_struct *p, unsigned long tls)
{
	struct user_desc __user *utls = (struct user_desc __user *)tls;

	if (in_ia32_syscall())
		return do_set_thread_area(p, -1, utls, 0);
	else
		return do_set_thread_area_64(p, ARCH_SET_FS, tls);
}

__visible void ret_from_fork(struct task_struct *prev, struct pt_regs *regs,
				     int (*fn)(void *), void *fn_arg)
{
	schedule_tail(prev);

	/* Is this a kernel thread? */
	if (unlikely(fn)) {
		fn(fn_arg);
		/*
		 * A kernel thread is allowed to return here after successfully
		 * calling kernel_execve().  Exit to userspace to complete the
		 * execve() syscall.
		 */
		regs->ax = 0;
	}

	syscall_exit_to_user_mode(regs);
}

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	unsigned long clone_flags = args->flags;
	unsigned long sp = args->stack;
	unsigned long tls = args->tls;
	struct inactive_task_frame *frame;
	struct fork_frame *fork_frame;
	struct pt_regs *childregs;
	unsigned long new_ssp;
	int ret = 0;

	childregs = task_pt_regs(p);
	fork_frame = container_of(childregs, struct fork_frame, regs);
	frame = &fork_frame->frame;

	frame->bp = encode_frame_pointer(childregs);
	frame->ret_addr = (unsigned long) ret_from_fork_asm;
	p->thread.sp = (unsigned long) fork_frame;
	p->thread.io_bitmap = NULL;
	clear_tsk_thread_flag(p, TIF_IO_BITMAP);
	p->thread.iopl_warn = 0;
	memset(p->thread.ptrace_bps, 0, sizeof(p->thread.ptrace_bps));

#ifdef CONFIG_X86_64
	current_save_fsgs();
	p->thread.fsindex = current->thread.fsindex;
	p->thread.fsbase = current->thread.fsbase;
	p->thread.gsindex = current->thread.gsindex;
	p->thread.gsbase = current->thread.gsbase;

	savesegment(es, p->thread.es);
	savesegment(ds, p->thread.ds);

	if (p->mm && (clone_flags & (CLONE_VM | CLONE_VFORK)) == CLONE_VM)
		set_bit(MM_CONTEXT_LOCK_LAM, &p->mm->context.flags);
#else
	p->thread.sp0 = (unsigned long) (childregs + 1);
	savesegment(gs, p->thread.gs);
	/*
	 * Clear all status flags including IF and set fixed bit. 64bit
	 * does not have this initialization as the frame does not contain
	 * flags. The flags consistency (especially vs. AC) is there
	 * ensured via objtool, which lacks 32bit support.
	 */
	frame->flags = X86_EFLAGS_FIXED;
#endif

	/*
	 * Allocate a new shadow stack for thread if needed. If shadow stack,
	 * is disabled, new_ssp will remain 0, and fpu_clone() will know not to
	 * update it.
	 */
	new_ssp = shstk_alloc_thread_stack(p, clone_flags, args->stack_size);
	if (IS_ERR_VALUE(new_ssp))
		return PTR_ERR((void *)new_ssp);

	fpu_clone(p, clone_flags, args->fn, new_ssp);

	/* Kernel thread ? */
	if (unlikely(p->flags & PF_KTHREAD)) {
		p->thread.pkru = pkru_get_init_value();
		memset(childregs, 0, sizeof(struct pt_regs));
		kthread_frame_init(frame, args->fn, args->fn_arg);
		return 0;
	}

	/*
	 * Clone current's PKRU value from hardware. tsk->thread.pkru
	 * is only valid when scheduled out.
	 */
	p->thread.pkru = read_pkru();

	frame->bx = 0;
	*childregs = *current_pt_regs();
	childregs->ax = 0;
	if (sp)
		childregs->sp = sp;

	if (unlikely(args->fn)) {
		/*
		 * A user space thread, but it doesn't return to
		 * ret_after_fork().
		 *
		 * In order to indicate that to tools like gdb,
		 * we reset the stack and instruction pointers.
		 *
		 * It does the same kernel frame setup to return to a kernel
		 * function that a kernel thread does.
		 */
		childregs->sp = 0;
		childregs->ip = 0;
		kthread_frame_init(frame, args->fn, args->fn_arg);
		return 0;
	}

	/* Set a new TLS for the child thread? */
	if (clone_flags & CLONE_SETTLS)
		ret = set_new_tls(p, tls);

	if (!ret && unlikely(test_tsk_thread_flag(current, TIF_IO_BITMAP)))
		io_bitmap_share(p);

	return ret;
}

static void pkru_flush_thread(void)
{
	/*
	 * If PKRU is enabled the default PKRU value has to be loaded into
	 * the hardware right here (similar to context switch).
	 */
	pkru_write_default();
}

void flush_thread(void)
{
	struct task_struct *tsk = current;

	flush_ptrace_hw_breakpoint(tsk);
	memset(tsk->thread.tls_array, 0, sizeof(tsk->thread.tls_array));

	fpu_flush_thread();
	pkru_flush_thread();
}

void disable_TSC(void)
{
	preempt_disable();
	if (!test_and_set_thread_flag(TIF_NOTSC))
		/*
		 * Must flip the CPU state synchronously with
		 * TIF_NOTSC in the current running context.
		 */
		cr4_set_bits(X86_CR4_TSD);
	preempt_enable();
}

static void enable_TSC(void)
{
	preempt_disable();
	if (test_and_clear_thread_flag(TIF_NOTSC))
		/*
		 * Must flip the CPU state synchronously with
		 * TIF_NOTSC in the current running context.
		 */
		cr4_clear_bits(X86_CR4_TSD);
	preempt_enable();
}

int get_tsc_mode(unsigned long adr)
{
	unsigned int val;

	if (test_thread_flag(TIF_NOTSC))
		val = PR_TSC_SIGSEGV;
	else
		val = PR_TSC_ENABLE;

	return put_user(val, (unsigned int __user *)adr);
}

int set_tsc_mode(unsigned int val)
{
	if (val == PR_TSC_SIGSEGV)
		disable_TSC();
	else if (val == PR_TSC_ENABLE)
		enable_TSC();
	else
		return -EINVAL;

	return 0;
}

DEFINE_PER_CPU(u64, msr_misc_features_shadow);

static void set_cpuid_faulting(bool on)
{
	u64 msrval;

	msrval = this_cpu_read(msr_misc_features_shadow);
	msrval &= ~MSR_MISC_FEATURES_ENABLES_CPUID_FAULT;
	msrval |= (on << MSR_MISC_FEATURES_ENABLES_CPUID_FAULT_BIT);
	this_cpu_write(msr_misc_features_shadow, msrval);
	wrmsrl(MSR_MISC_FEATURES_ENABLES, msrval);
}

static void disable_cpuid(void)
{
	preempt_disable();
	if (!test_and_set_thread_flag(TIF_NOCPUID)) {
		/*
		 * Must flip the CPU state synchronously with
		 * TIF_NOCPUID in the current running context.
		 */
		set_cpuid_faulting(true);
	}
	preempt_enable();
}

static void enable_cpuid(void)
{
	preempt_disable();
	if (test_and_clear_thread_flag(TIF_NOCPUID)) {
		/*
		 * Must flip the CPU state synchronously with
		 * TIF_NOCPUID in the current running context.
		 */
		set_cpuid_faulting(false);
	}
	preempt_enable();
}

static int get_cpuid_mode(void)
{
	return !test_thread_flag(TIF_NOCPUID);
}

static int set_cpuid_mode(unsigned long cpuid_enabled)
{
	if (!boot_cpu_has(X86_FEATURE_CPUID_FAULT))
		return -ENODEV;

	if (cpuid_enabled)
		enable_cpuid();
	else
		disable_cpuid();

	return 0;
}

/*
 * Called immediately after a successful exec.
 */
void arch_setup_new_exec(void)
{
	/* If cpuid was previously disabled for this task, re-enable it. */
	if (test_thread_flag(TIF_NOCPUID))
		enable_cpuid();

	/*
	 * Don't inherit TIF_SSBD across exec boundary when
	 * PR_SPEC_DISABLE_NOEXEC is used.
	 */
	if (test_thread_flag(TIF_SSBD) &&
	    task_spec_ssb_noexec(current)) {
		clear_thread_flag(TIF_SSBD);
		task_clear_spec_ssb_disable(current);
		task_clear_spec_ssb_noexec(current);
		speculation_ctrl_update(read_thread_flags());
	}

	mm_reset_untag_mask(current->mm);
}

#ifdef CONFIG_X86_IOPL_IOPERM
static inline void switch_to_bitmap(unsigned long tifp)
{
	/*
	 * Invalidate I/O bitmap if the previous task used it. This prevents
	 * any possible leakage of an active I/O bitmap.
	 *
	 * If the next task has an I/O bitmap it will handle it on exit to
	 * user mode.
	 */
	if (tifp & _TIF_IO_BITMAP)
		tss_invalidate_io_bitmap();
}

static void tss_copy_io_bitmap(struct tss_struct *tss, struct io_bitmap *iobm)
{
	/*
	 * Copy at least the byte range of the incoming tasks bitmap which
	 * covers the permitted I/O ports.
	 *
	 * If the previous task which used an I/O bitmap had more bits
	 * permitted, then the copy needs to cover those as well so they
	 * get turned off.
	 */
	memcpy(tss->io_bitmap.bitmap, iobm->bitmap,
	       max(tss->io_bitmap.prev_max, iobm->max));

	/*
	 * Store the new max and the sequence number of this bitmap
	 * and a pointer to the bitmap itself.
	 */
	tss->io_bitmap.prev_max = iobm->max;
	tss->io_bitmap.prev_sequence = iobm->sequence;
}

/**
 * native_tss_update_io_bitmap - Update I/O bitmap before exiting to user mode
 */
void native_tss_update_io_bitmap(void)
{
	struct tss_struct *tss = this_cpu_ptr(&cpu_tss_rw);
	struct thread_struct *t = &current->thread;
	u16 *base = &tss->x86_tss.io_bitmap_base;

	if (!test_thread_flag(TIF_IO_BITMAP)) {
		native_tss_invalidate_io_bitmap();
		return;
	}

	if (IS_ENABLED(CONFIG_X86_IOPL_IOPERM) && t->iopl_emul == 3) {
		*base = IO_BITMAP_OFFSET_VALID_ALL;
	} else {
		struct io_bitmap *iobm = t->io_bitmap;

		if (WARN_ON_ONCE(!iobm)) {
			clear_thread_flag(TIF_IO_BITMAP);
			native_tss_invalidate_io_bitmap();
		}

		/*
		 * Only copy bitmap data when the sequence number differs. The
		 * update time is accounted to the incoming task.
		 */
		if (tss->io_bitmap.prev_sequence != iobm->sequence)
			tss_copy_io_bitmap(tss, iobm);

		/* Enable the bitmap */
		*base = IO_BITMAP_OFFSET_VALID_MAP;
	}

	/*
	 * Make sure that the TSS limit is covering the IO bitmap. It might have
	 * been cut down by a VMEXIT to 0x67 which would cause a subsequent I/O
	 * access from user space to trigger a #GP because the bitmap is outside
	 * the TSS limit.
	 */
	refresh_tss_limit();
}
#else /* CONFIG_X86_IOPL_IOPERM */
static inline void switch_to_bitmap(unsigned long tifp) { }
#endif

#ifdef CONFIG_SMP

struct ssb_state {
	struct ssb_state	*shared_state;
	raw_spinlock_t		lock;
	unsigned int		disable_state;
	unsigned long		local_state;
};

#define LSTATE_SSB	0

static DEFINE_PER_CPU(struct ssb_state, ssb_state);

void speculative_store_bypass_ht_init(void)
{
	struct ssb_state *st = this_cpu_ptr(&ssb_state);
	unsigned int this_cpu = smp_processor_id();
	unsigned int cpu;

	st->local_state = 0;

	/*
	 * Shared state setup happens once on the first bringup
	 * of the CPU. It's not destroyed on CPU hotunplug.
	 */
	if (st->shared_state)
		return;

	raw_spin_lock_init(&st->lock);

	/*
	 * Go over HT siblings and check whether one of them has set up the
	 * shared state pointer already.
	 */
	for_each_cpu(cpu, topology_sibling_cpumask(this_cpu)) {
		if (cpu == this_cpu)
			continue;

		if (!per_cpu(ssb_state, cpu).shared_state)
			continue;

		/* Link it to the state of the sibling: */
		st->shared_state = per_cpu(ssb_state, cpu).shared_state;
		return;
	}

	/*
	 * First HT sibling to come up on the core.  Link shared state of
	 * the first HT sibling to itself. The siblings on the same core
	 * which come up later will see the shared state pointer and link
	 * themselves to the state of this CPU.
	 */
	st->shared_state = st;
}

/*
 * Logic is: First HT sibling enables SSBD for both siblings in the core
 * and last sibling to disable it, disables it for the whole core. This how
 * MSR_SPEC_CTRL works in "hardware":
 *
 *  CORE_SPEC_CTRL = THREAD0_SPEC_CTRL | THREAD1_SPEC_CTRL
 */
static __always_inline void amd_set_core_ssb_state(unsigned long tifn)
{
	struct ssb_state *st = this_cpu_ptr(&ssb_state);
	u64 msr = x86_amd_ls_cfg_base;

	if (!static_cpu_has(X86_FEATURE_ZEN)) {
		msr |= ssbd_tif_to_amd_ls_cfg(tifn);
		wrmsrl(MSR_AMD64_LS_CFG, msr);
		return;
	}

	if (tifn & _TIF_SSBD) {
		/*
		 * Since this can race with prctl(), block reentry on the
		 * same CPU.
		 */
		if (__test_and_set_bit(LSTATE_SSB, &st->local_state))
			return;

		msr |= x86_amd_ls_cfg_ssbd_mask;

		raw_spin_lock(&st->shared_state->lock);
		/* First sibling enables SSBD: */
		if (!st->shared_state->disable_state)
			wrmsrl(MSR_AMD64_LS_CFG, msr);
		st->shared_state->disable_state++;
		raw_spin_unlock(&st->shared_state->lock);
	} else {
		if (!__test_and_clear_bit(LSTATE_SSB, &st->local_state))
			return;

		raw_spin_lock(&st->shared_state->lock);
		st->shared_state->disable_state--;
		if (!st->shared_state->disable_state)
			wrmsrl(MSR_AMD64_LS_CFG, msr);
		raw_spin_unlock(&st->shared_state->lock);
	}
}
#else
static __always_inline void amd_set_core_ssb_state(unsigned long tifn)
{
	u64 msr = x86_amd_ls_cfg_base | ssbd_tif_to_amd_ls_cfg(tifn);

	wrmsrl(MSR_AMD64_LS_CFG, msr);
}
#endif

static __always_inline void amd_set_ssb_virt_state(unsigned long tifn)
{
	/*
	 * SSBD has the same definition in SPEC_CTRL and VIRT_SPEC_CTRL,
	 * so ssbd_tif_to_spec_ctrl() just works.
	 */
	wrmsrl(MSR_AMD64_VIRT_SPEC_CTRL, ssbd_tif_to_spec_ctrl(tifn));
}

/*
 * Update the MSRs managing speculation control, during context switch.
 *
 * tifp: Previous task's thread flags
 * tifn: Next task's thread flags
 */
static __always_inline void __speculation_ctrl_update(unsigned long tifp,
						      unsigned long tifn)
{
	unsigned long tif_diff = tifp ^ tifn;
	u64 msr = x86_spec_ctrl_base;
	bool updmsr = false;

	lockdep_assert_irqs_disabled();

	/* Handle change of TIF_SSBD depending on the mitigation method. */
	if (static_cpu_has(X86_FEATURE_VIRT_SSBD)) {
		if (tif_diff & _TIF_SSBD)
			amd_set_ssb_virt_state(tifn);
	} else if (static_cpu_has(X86_FEATURE_LS_CFG_SSBD)) {
		if (tif_diff & _TIF_SSBD)
			amd_set_core_ssb_state(tifn);
	} else if (static_cpu_has(X86_FEATURE_SPEC_CTRL_SSBD) ||
		   static_cpu_has(X86_FEATURE_AMD_SSBD)) {
		updmsr |= !!(tif_diff & _TIF_SSBD);
		msr |= ssbd_tif_to_spec_ctrl(tifn);
	}

	/* Only evaluate TIF_SPEC_IB if conditional STIBP is enabled. */
	if (IS_ENABLED(CONFIG_SMP) &&
	    static_branch_unlikely(&switch_to_cond_stibp)) {
		updmsr |= !!(tif_diff & _TIF_SPEC_IB);
		msr |= stibp_tif_to_spec_ctrl(tifn);
	}

	if (updmsr)
		update_spec_ctrl_cond(msr);
}

static unsigned long speculation_ctrl_update_tif(struct task_struct *tsk)
{
	if (test_and_clear_tsk_thread_flag(tsk, TIF_SPEC_FORCE_UPDATE)) {
		if (task_spec_ssb_disable(tsk))
			set_tsk_thread_flag(tsk, TIF_SSBD);
		else
			clear_tsk_thread_flag(tsk, TIF_SSBD);

		if (task_spec_ib_disable(tsk))
			set_tsk_thread_flag(tsk, TIF_SPEC_IB);
		else
			clear_tsk_thread_flag(tsk, TIF_SPEC_IB);
	}
	/* Return the updated threadinfo flags*/
	return read_task_thread_flags(tsk);
}

void speculation_ctrl_update(unsigned long tif)
{
	unsigned long flags;

	/* Forced update. Make sure all relevant TIF flags are different */
	local_irq_save(flags);
	__speculation_ctrl_update(~tif, tif);
	local_irq_restore(flags);
}

/* Called from seccomp/prctl update */
void speculation_ctrl_update_current(void)
{
	preempt_disable();
	speculation_ctrl_update(speculation_ctrl_update_tif(current));
	preempt_enable();
}

static inline void cr4_toggle_bits_irqsoff(unsigned long mask)
{
	unsigned long newval, cr4 = this_cpu_read(cpu_tlbstate.cr4);

	newval = cr4 ^ mask;
	if (newval != cr4) {
		this_cpu_write(cpu_tlbstate.cr4, newval);
		__write_cr4(newval);
	}
}

void __switch_to_xtra(struct task_struct *prev_p, struct task_struct *next_p)
{
	unsigned long tifp, tifn;

	tifn = read_task_thread_flags(next_p);
	tifp = read_task_thread_flags(prev_p);

	switch_to_bitmap(tifp);

	propagate_user_return_notify(prev_p, next_p);

	if ((tifp & _TIF_BLOCKSTEP || tifn & _TIF_BLOCKSTEP) &&
	    arch_has_block_step()) {
		unsigned long debugctl, msk;

		rdmsrl(MSR_IA32_DEBUGCTLMSR, debugctl);
		debugctl &= ~DEBUGCTLMSR_BTF;
		msk = tifn & _TIF_BLOCKSTEP;
		debugctl |= (msk >> TIF_BLOCKSTEP) << DEBUGCTLMSR_BTF_SHIFT;
		wrmsrl(MSR_IA32_DEBUGCTLMSR, debugctl);
	}

	if ((tifp ^ tifn) & _TIF_NOTSC)
		cr4_toggle_bits_irqsoff(X86_CR4_TSD);

	if ((tifp ^ tifn) & _TIF_NOCPUID)
		set_cpuid_faulting(!!(tifn & _TIF_NOCPUID));

	if (likely(!((tifp | tifn) & _TIF_SPEC_FORCE_UPDATE))) {
		__speculation_ctrl_update(tifp, tifn);
	} else {
		speculation_ctrl_update_tif(prev_p);
		tifn = speculation_ctrl_update_tif(next_p);

		/* Enforce MSR update to ensure consistent state */
		__speculation_ctrl_update(~tifn, tifn);
	}
}

/*
 * Idle related variables and functions
 */
unsigned long boot_option_idle_override = IDLE_NO_OVERRIDE;
EXPORT_SYMBOL(boot_option_idle_override);

/*
 * We use this if we don't have any better idle routine..
 */
void __cpuidle default_idle(void)
{
	raw_safe_halt();
	raw_local_irq_disable();
}
#if defined(CONFIG_APM_MODULE) || defined(CONFIG_HALTPOLL_CPUIDLE_MODULE)
EXPORT_SYMBOL(default_idle);
#endif

DEFINE_STATIC_CALL_NULL(x86_idle, default_idle);

static bool x86_idle_set(void)
{
	return !!static_call_query(x86_idle);
}

#ifndef CONFIG_SMP
static inline void __noreturn play_dead(void)
{
	BUG();
}
#endif

void arch_cpu_idle_enter(void)
{
	tsc_verify_tsc_adjust(false);
	local_touch_nmi();
}

void __noreturn arch_cpu_idle_dead(void)
{
	play_dead();
}

/*
 * Called from the generic idle code.
 */
void __cpuidle arch_cpu_idle(void)
{
	static_call(x86_idle)();
}
EXPORT_SYMBOL_GPL(arch_cpu_idle);

#ifdef CONFIG_XEN
bool xen_set_default_idle(void)
{
	bool ret = x86_idle_set();

	static_call_update(x86_idle, default_idle);

	return ret;
}
#endif

struct cpumask cpus_stop_mask;

void __noreturn stop_this_cpu(void *dummy)
{
	struct cpuinfo_x86 *c = this_cpu_ptr(&cpu_info);
	unsigned int cpu = smp_processor_id();

	local_irq_disable();

	/*
	 * Remove this CPU from the online mask and disable it
	 * unconditionally. This might be redundant in case that the reboot
	 * vector was handled late and stop_other_cpus() sent an NMI.
	 *
	 * According to SDM and APM NMIs can be accepted even after soft
	 * disabling the local APIC.
	 */
	set_cpu_online(cpu, false);
	disable_local_APIC();
	mcheck_cpu_clear(c);

	/*
	 * Use wbinvd on processors that support SME. This provides support
	 * for performing a successful kexec when going from SME inactive
	 * to SME active (or vice-versa). The cache must be cleared so that
	 * if there are entries with the same physical address, both with and
	 * without the encryption bit, they don't race each other when flushed
	 * and potentially end up with the wrong entry being committed to
	 * memory.
	 *
	 * Test the CPUID bit directly because the machine might've cleared
	 * X86_FEATURE_SME due to cmdline options.
	 */
	if (c->extended_cpuid_level >= 0x8000001f && (cpuid_eax(0x8000001f) & BIT(0)))
		wbinvd();

	/*
	 * This brings a cache line back and dirties it, but
	 * native_stop_other_cpus() will overwrite cpus_stop_mask after it
	 * observed that all CPUs reported stop. This write will invalidate
	 * the related cache line on this CPU.
	 */
	cpumask_clear_cpu(cpu, &cpus_stop_mask);

#ifdef CONFIG_SMP
	if (smp_ops.stop_this_cpu) {
		smp_ops.stop_this_cpu();
		BUG();
	}
#endif

	for (;;) {
		/*
		 * Use native_halt() so that memory contents don't change
		 * (stack usage and variables) after possibly issuing the
		 * wbinvd() above.
		 */
		native_halt();
	}
}

/*
 * Prefer MWAIT over HALT if MWAIT is supported, MWAIT_CPUID leaf
 * exists and whenever MONITOR/MWAIT extensions are present there is at
 * least one C1 substate.
 *
 * Do not prefer MWAIT if MONITOR instruction has a bug or idle=nomwait
 * is passed to kernel commandline parameter.
 */
static __init bool prefer_mwait_c1_over_halt(void)
{
	const struct cpuinfo_x86 *c = &boot_cpu_data;
	u32 eax, ebx, ecx, edx;

	/* If override is enforced on the command line, fall back to HALT. */
	if (boot_option_idle_override != IDLE_NO_OVERRIDE)
		return false;

	/* MWAIT is not supported on this platform. Fallback to HALT */
	if (!cpu_has(c, X86_FEATURE_MWAIT))
		return false;

	/* Monitor has a bug or APIC stops in C1E. Fallback to HALT */
	if (boot_cpu_has_bug(X86_BUG_MONITOR) || boot_cpu_has_bug(X86_BUG_AMD_APIC_C1E))
		return false;

	cpuid(CPUID_LEAF_MWAIT, &eax, &ebx, &ecx, &edx);

	/*
	 * If MWAIT extensions are not available, it is safe to use MWAIT
	 * with EAX=0, ECX=0.
	 */
	if (!(ecx & CPUID5_ECX_EXTENSIONS_SUPPORTED))
		return true;

	/*
	 * If MWAIT extensions are available, there should be at least one
	 * MWAIT C1 substate present.
	 */
	return !!(edx & MWAIT_C1_SUBSTATE_MASK);
}

/*
 * MONITOR/MWAIT with no hints, used for default C1 state. This invokes MWAIT
 * with interrupts enabled and no flags, which is backwards compatible with the
 * original MWAIT implementation.
 */
static __cpuidle void mwait_idle(void)
{
	if (need_resched())
		return;

	x86_idle_clear_cpu_buffers();

	if (!current_set_polling_and_test()) {
		const void *addr = &current_thread_info()->flags;

		alternative_input("", "clflush (%[addr])", X86_BUG_CLFLUSH_MONITOR, [addr] "a" (addr));
		__monitor(addr, 0, 0);
		if (need_resched())
			goto out;

		__sti_mwait(0, 0);
		raw_local_irq_disable();
	}

out:
	__current_clr_polling();
}

void __init select_idle_routine(void)
{
	if (boot_option_idle_override == IDLE_POLL) {
		if (IS_ENABLED(CONFIG_SMP) && __max_threads_per_core > 1)
			pr_warn_once("WARNING: polling idle and HT enabled, performance may degrade\n");
		return;
	}

	/* Required to guard against xen_set_default_idle() */
	if (x86_idle_set())
		return;

	if (prefer_mwait_c1_over_halt()) {
		pr_info("using mwait in idle threads\n");
		static_call_update(x86_idle, mwait_idle);
	} else if (cpu_feature_enabled(X86_FEATURE_TDX_GUEST)) {
		pr_info("using TDX aware idle routine\n");
		static_call_update(x86_idle, tdx_halt);
	} else {
		static_call_update(x86_idle, default_idle);
	}
}

void amd_e400_c1e_apic_setup(void)
{
	if (boot_cpu_has_bug(X86_BUG_AMD_APIC_C1E)) {
		pr_info("Switch to broadcast mode on CPU%d\n", smp_processor_id());
		local_irq_disable();
		tick_broadcast_force();
		local_irq_enable();
	}
}

void __init arch_post_acpi_subsys_init(void)
{
	u32 lo, hi;

	if (!boot_cpu_has_bug(X86_BUG_AMD_E400))
		return;

	/*
	 * AMD E400 detection needs to happen after ACPI has been enabled. If
	 * the machine is affected K8_INTP_C1E_ACTIVE_MASK bits are set in
	 * MSR_K8_INT_PENDING_MSG.
	 */
	rdmsr(MSR_K8_INT_PENDING_MSG, lo, hi);
	if (!(lo & K8_INTP_C1E_ACTIVE_MASK))
		return;

	boot_cpu_set_bug(X86_BUG_AMD_APIC_C1E);

	if (!boot_cpu_has(X86_FEATURE_NONSTOP_TSC))
		mark_tsc_unstable("TSC halt in AMD C1E");

	if (IS_ENABLED(CONFIG_GENERIC_CLOCKEVENTS_BROADCAST_IDLE))
		static_branch_enable(&arch_needs_tick_broadcast);
	pr_info("System has AMD C1E erratum E400. Workaround enabled.\n");
}

static int __init idle_setup(char *str)
{
	if (!str)
		return -EINVAL;

	if (!strcmp(str, "poll")) {
		pr_info("using polling idle threads\n");
		boot_option_idle_override = IDLE_POLL;
		cpu_idle_poll_ctrl(true);
	} else if (!strcmp(str, "halt")) {
		/* 'idle=halt' HALT for idle. C-states are disabled. */
		boot_option_idle_override = IDLE_HALT;
	} else if (!strcmp(str, "nomwait")) {
		/* 'idle=nomwait' disables MWAIT for idle */
		boot_option_idle_override = IDLE_NOMWAIT;
	} else {
		return -EINVAL;
	}

	return 0;
}
early_param("idle", idle_setup);

unsigned long arch_align_stack(unsigned long sp)
{
	if (!(current->personality & ADDR_NO_RANDOMIZE) && randomize_va_space)
		sp -= get_random_u32_below(8192);
	return sp & ~0xf;
}

unsigned long arch_randomize_brk(struct mm_struct *mm)
{
	if (mmap_is_ia32())
		return randomize_page(mm->brk, SZ_32M);

	return randomize_page(mm->brk, SZ_1G);
}

/*
 * Called from fs/proc with a reference on @p to find the function
 * which called into schedule(). This needs to be done carefully
 * because the task might wake up and we might look at a stack
 * changing under us.
 */
unsigned long __get_wchan(struct task_struct *p)
{
	struct unwind_state state;
	unsigned long addr = 0;

	if (!try_get_task_stack(p))
		return 0;

	for (unwind_start(&state, p, NULL, NULL); !unwind_done(&state);
	     unwind_next_frame(&state)) {
		addr = unwind_get_return_address(&state);
		if (!addr)
			break;
		if (in_sched_functions(addr))
			continue;
		break;
	}

	put_task_stack(p);

	return addr;
}

SYSCALL_DEFINE2(arch_prctl, int, option, unsigned long, arg2)
{
	switch (option) {
	case ARCH_GET_CPUID:
		return get_cpuid_mode();
	case ARCH_SET_CPUID:
		return set_cpuid_mode(arg2);
	case ARCH_GET_XCOMP_SUPP:
	case ARCH_GET_XCOMP_PERM:
	case ARCH_REQ_XCOMP_PERM:
	case ARCH_GET_XCOMP_GUEST_PERM:
	case ARCH_REQ_XCOMP_GUEST_PERM:
		return fpu_xstate_prctl(option, arg2);
	}

	if (!in_ia32_syscall())
		return do_arch_prctl_64(current, option, arg2);

	return -EINVAL;
}

SYSCALL_DEFINE0(ni_syscall)
{
	return -ENOSYS;
}
