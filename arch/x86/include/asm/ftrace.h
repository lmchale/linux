/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_FTRACE_H
#define _ASM_X86_FTRACE_H

#include <asm/ptrace.h>

#ifdef CONFIG_FUNCTION_TRACER
#ifndef CC_USING_FENTRY
# error Compiler does not support fentry?
#endif
# define MCOUNT_ADDR		((unsigned long)(__fentry__))
#define MCOUNT_INSN_SIZE	5 /* sizeof mcount call */

/* Ignore unused weak functions which will have non zero offsets */
#ifdef CONFIG_HAVE_FENTRY
# include <asm/ibt.h>
/* Add offset for endbr64 if IBT enabled */
# define FTRACE_MCOUNT_MAX_OFFSET	ENDBR_INSN_SIZE
#endif

#ifdef CONFIG_DYNAMIC_FTRACE
#define ARCH_SUPPORTS_FTRACE_OPS 1
#endif

#ifndef __ASSEMBLER__
extern void __fentry__(void);

static inline unsigned long ftrace_call_adjust(unsigned long addr)
{
	/*
	 * addr is the address of the mcount call instruction.
	 * recordmcount does the necessary offset calculation.
	 */
	return addr;
}

static inline unsigned long arch_ftrace_get_symaddr(unsigned long fentry_ip)
{
#ifdef CONFIG_X86_KERNEL_IBT
	u32 instr;

	/* We want to be extra safe in case entry ip is on the page edge,
	 * but otherwise we need to avoid get_kernel_nofault()'s overhead.
	 */
	if ((fentry_ip & ~PAGE_MASK) < ENDBR_INSN_SIZE) {
		if (get_kernel_nofault(instr, (u32 *)(fentry_ip - ENDBR_INSN_SIZE)))
			return fentry_ip;
	} else {
		instr = *(u32 *)(fentry_ip - ENDBR_INSN_SIZE);
	}
	if (is_endbr(instr))
		fentry_ip -= ENDBR_INSN_SIZE;
#endif
	return fentry_ip;
}
#define ftrace_get_symaddr(fentry_ip)	arch_ftrace_get_symaddr(fentry_ip)

#ifdef CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS

#include <linux/ftrace_regs.h>

static __always_inline struct pt_regs *
arch_ftrace_get_regs(struct ftrace_regs *fregs)
{
	/* Only when FL_SAVE_REGS is set, cs will be non zero */
	if (!arch_ftrace_regs(fregs)->regs.cs)
		return NULL;
	return &arch_ftrace_regs(fregs)->regs;
}

#define arch_ftrace_fill_perf_regs(fregs, _regs) do {	\
		(_regs)->ip = arch_ftrace_regs(fregs)->regs.ip;		\
		(_regs)->sp = arch_ftrace_regs(fregs)->regs.sp;		\
		(_regs)->cs = __KERNEL_CS;		\
		(_regs)->flags = 0;			\
	} while (0)

#define ftrace_regs_set_instruction_pointer(fregs, _ip)	\
	do { arch_ftrace_regs(fregs)->regs.ip = (_ip); } while (0)


static __always_inline unsigned long
ftrace_regs_get_return_address(struct ftrace_regs *fregs)
{
	return *(unsigned long *)ftrace_regs_get_stack_pointer(fregs);
}

struct ftrace_ops;
#define ftrace_graph_func ftrace_graph_func
void ftrace_graph_func(unsigned long ip, unsigned long parent_ip,
		       struct ftrace_ops *op, struct ftrace_regs *fregs);
#else
#define FTRACE_GRAPH_TRAMP_ADDR FTRACE_GRAPH_ADDR
#endif

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS
/*
 * When a ftrace registered caller is tracing a function that is
 * also set by a register_ftrace_direct() call, it needs to be
 * differentiated in the ftrace_caller trampoline. To do this, we
 * place the direct caller in the ORIG_AX part of pt_regs. This
 * tells the ftrace_caller that there's a direct caller.
 */
static inline void
__arch_ftrace_set_direct_caller(struct pt_regs *regs, unsigned long addr)
{
	/* Emulate a call */
	regs->orig_ax = addr;
}
#define arch_ftrace_set_direct_caller(fregs, addr) \
	__arch_ftrace_set_direct_caller(&arch_ftrace_regs(fregs)->regs, addr)
#endif /* CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS */

#ifdef CONFIG_DYNAMIC_FTRACE

struct dyn_arch_ftrace {
	/* No extra data needed for x86 */
};

#endif /*  CONFIG_DYNAMIC_FTRACE */
#endif /* __ASSEMBLER__ */
#endif /* CONFIG_FUNCTION_TRACER */


#ifndef __ASSEMBLER__

void prepare_ftrace_return(unsigned long ip, unsigned long *parent,
			   unsigned long frame_pointer);

#if defined(CONFIG_FUNCTION_TRACER) && defined(CONFIG_DYNAMIC_FTRACE)
extern void set_ftrace_ops_ro(void);
#else
static inline void set_ftrace_ops_ro(void) { }
#endif

#define ARCH_HAS_SYSCALL_MATCH_SYM_NAME
static inline bool arch_syscall_match_sym_name(const char *sym, const char *name)
{
	/*
	 * Compare the symbol name with the system call name. Skip the
	 * "__x64_sys", "__ia32_sys", "__do_sys" or simple "sys" prefix.
	 */
	return !strcmp(sym + 3, name + 3) ||
		(!strncmp(sym, "__x64_", 6) && !strcmp(sym + 9, name + 3)) ||
		(!strncmp(sym, "__ia32_", 7) && !strcmp(sym + 10, name + 3)) ||
		(!strncmp(sym, "__do_sys", 8) && !strcmp(sym + 8, name + 3));
}

#ifndef COMPILE_OFFSETS

#if defined(CONFIG_FTRACE_SYSCALLS) && defined(CONFIG_IA32_EMULATION)
#include <linux/compat.h>

/*
 * Because ia32 syscalls do not map to x86_64 syscall numbers
 * this screws up the trace output when tracing a ia32 task.
 * Instead of reporting bogus syscalls, just do not trace them.
 *
 * If the user really wants these, then they should use the
 * raw syscall tracepoints with filtering.
 */
#define ARCH_TRACE_IGNORE_COMPAT_SYSCALLS 1
static inline bool arch_trace_is_compat_syscall(struct pt_regs *regs)
{
	return in_32bit_syscall();
}
#endif /* CONFIG_FTRACE_SYSCALLS && CONFIG_IA32_EMULATION */
#endif /* !COMPILE_OFFSETS */
#endif /* !__ASSEMBLER__ */

#endif /* _ASM_X86_FTRACE_H */
