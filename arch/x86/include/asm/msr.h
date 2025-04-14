/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MSR_H
#define _ASM_X86_MSR_H

#include "msr-index.h"

#ifndef __ASSEMBLER__

#include <asm/asm.h>
#include <asm/errno.h>
#include <asm/cpufeature.h>
#include <asm/cpumask.h>
#include <uapi/asm/msr.h>
#include <asm/shared/msr.h>

#include <linux/percpu.h>

struct msr_info {
	u32			msr_no;
	struct msr		reg;
	struct msr __percpu	*msrs;
	int			err;
};

struct msr_regs_info {
	u32 *regs;
	int err;
};

struct saved_msr {
	bool valid;
	struct msr_info info;
};

struct saved_msrs {
	unsigned int num;
	struct saved_msr *array;
};

/*
 * both i386 and x86_64 returns 64-bit value in edx:eax, but gcc's "A"
 * constraint has different meanings. For i386, "A" means exactly
 * edx:eax, while for x86_64 it doesn't mean rdx:rax or edx:eax. Instead,
 * it means rax *or* rdx.
 */
#ifdef CONFIG_X86_64
/* Using 64-bit values saves one instruction clearing the high half of low */
#define DECLARE_ARGS(val, low, high)	unsigned long low, high
#define EAX_EDX_VAL(val, low, high)	((low) | (high) << 32)
#define EAX_EDX_RET(val, low, high)	"=a" (low), "=d" (high)
#else
#define DECLARE_ARGS(val, low, high)	u64 val
#define EAX_EDX_VAL(val, low, high)	(val)
#define EAX_EDX_RET(val, low, high)	"=A" (val)
#endif

/*
 * Be very careful with includes. This header is prone to include loops.
 */
#include <asm/atomic.h>
#include <linux/tracepoint-defs.h>

#ifdef CONFIG_TRACEPOINTS
DECLARE_TRACEPOINT(read_msr);
DECLARE_TRACEPOINT(write_msr);
DECLARE_TRACEPOINT(rdpmc);
extern void do_trace_write_msr(u32 msr, u64 val, int failed);
extern void do_trace_read_msr(u32 msr, u64 val, int failed);
extern void do_trace_rdpmc(u32 msr, u64 val, int failed);
#else
static inline void do_trace_write_msr(u32 msr, u64 val, int failed) {}
static inline void do_trace_read_msr(u32 msr, u64 val, int failed) {}
static inline void do_trace_rdpmc(u32 msr, u64 val, int failed) {}
#endif

#ifdef CONFIG_XEN_PV
extern void asm_xen_read_msr(void);
extern void asm_xen_write_msr(void);
extern u64 xen_read_pmc(int counter);

enum pv_msr_action {
	PV_MSR_PV,
	PV_MSR_IGNORE,
};

/* Is there a better header to place this function? */
static __always_inline enum pv_msr_action get_pv_msr_action(u32 msr)
{
	switch (msr) {
	case MSR_STAR:
	case MSR_CSTAR:
	case MSR_LSTAR:
	case MSR_SYSCALL_MASK:
	case MSR_IA32_SYSENTER_CS:
	case MSR_IA32_SYSENTER_ESP:
	case MSR_IA32_SYSENTER_EIP:
		/* Fast syscall setup is all done in hypercalls, so
		   these are all ignored.  Stub them out here to stop
		   Xen console noise. */
		return PV_MSR_IGNORE;

	default:
		return PV_MSR_PV;
	}
}
#endif

/* Instruction opcode for WRMSRNS supported in binutils >= 2.40 */
#define ASM_WRMSRNS		_ASM_BYTES(0x0f,0x01,0xc6)

/*
 * The GNU assembler supports the .insn directive since version 2.41.
 */
#if defined(CONFIG_AS_IS_GNU) && CONFIG_AS_VERSION >= 24100
#define ASM_RDMSR_IMM			\
	" .insn VEX.128.F2.M7.W0 0xf6 /0, %[msr]%{:u32}, %[val]\n\t"
#define ASM_WRMSRNS_IMM			\
	" .insn VEX.128.F3.M7.W0 0xf6 /0, %[val], %[msr]%{:u32}\n\t"
#else
/*
 * Note, clang also doesn't support the .insn directive.
 *
 * The register operand is encoded as %rax because all uses of the immediate
 * form MSR access instructions reference %rax as the register operand.
 */
#define ASM_RDMSR_IMM			\
	" .byte 0xc4,0xe7,0x7b,0xf6,0xc0; .long %c[msr]"
#define ASM_WRMSRNS_IMM			\
	" .byte 0xc4,0xe7,0x7a,0xf6,0xc0; .long %c[msr]"
#endif

#define RDMSR_AND_SAVE_RESULT		\
	"rdmsr\n\t"			\
	"shl $0x20, %%rdx\n\t"		\
	"or %%rdx, %%rax\n\t"

#define PREPARE_RDX_FOR_WRMSR		\
	"mov %%rax, %%rdx\n\t"		\
	"shr $0x20, %%rdx\n\t"

#define PREPARE_RCX_RDX_FOR_WRMSR	\
	"mov %[msr], %%ecx\n\t"		\
	PREPARE_RDX_FOR_WRMSR

/*
 * There are two sets of functions for MSR accesses, the native ones and
 * the generic ones.  The native ones are used when the code knows that
 * it can only run on a native CPU.  The generic ones are used when the
 * code could run on a paravirtualized CPU and a native CPU.
 *
 * When the compiler can determine the MSR number at compile time, the APIs
 * with the suffix _constant() are used to enable the immediate form MSR
 * instructions when available.  The APIs with the suffix _variable() are
 * used when the MSR number is not known until run time.
 *
 * Below is a diagram illustrating the derivation of the MSR read APIs:
 *
 *      __native_rdmsr_variable()    __native_rdmsr_constant()
 *                         \           /
 *                          \         /
 *                         __native_rdmsr()   ------------------------
 *                            /     \                                |
 *                           /       \                               |
 *               native_rdmsrq()    native_read_msr_safe()           |
 *                   /    \                                          |
 *                  /      \                                         |
 *      native_rdmsr()    native_read_msr()                          |
 *                                                                   |
 *                                                                   |
 *                                                                   |
 *                    __xenpv_rdmsr()                                |
 *                         |                                         |
 *                         |                                         |
 *                      __rdmsr()   <---------------------------------
 *                       /    \
 *                      /      \
 *                 rdmsrq()   rdmsrq_safe()
 *                    /          \
 *                   /            \
 *                rdmsr()        rdmsr_safe()
 */

static __always_inline bool __native_rdmsr_variable(u32 msr, u64 *val, int type)
{
#ifdef CONFIG_X86_64
	BUILD_BUG_ON(__builtin_constant_p(msr));

	asm_inline volatile goto(
		"1:\n"
		RDMSR_AND_SAVE_RESULT
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For RDMSR */

		: [val] "=a" (*val)
		: "c" (msr), [type] "i" (type)
		: "memory", "rdx"
		: badmsr);
#else
	asm_inline volatile goto(
		"1: rdmsr\n\t"
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For RDMSR */

		: "=A" (*val)
		: "c" (msr), [type] "i" (type)
		: "memory"
		: badmsr);
#endif

	return false;

badmsr:
	return true;
}

#ifdef CONFIG_X86_64
static __always_inline bool __native_rdmsr_constant(u32 msr, u64 *val, int type)
{
	BUILD_BUG_ON(!__builtin_constant_p(msr));

	asm_inline volatile goto(
		"1:\n"
		ALTERNATIVE("mov %[msr], %%ecx\n\t"
			    "2:\n"
			    RDMSR_AND_SAVE_RESULT,
			    ASM_RDMSR_IMM,
			    X86_FEATURE_MSR_IMM)
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For RDMSR immediate */
		_ASM_EXTABLE_TYPE(2b, %l[badmsr], %c[type])	/* For RDMSR */

		: [val] "=a" (*val)
		: [msr] "i" (msr), [type] "i" (type)
		: "memory", "ecx", "rdx"
		: badmsr);

	return false;

badmsr:
	return true;
}
#endif

static __always_inline bool __native_rdmsr(u32 msr, u64 *val, int type)
{
#ifdef CONFIG_X86_64
	if (__builtin_constant_p(msr))
		return __native_rdmsr_constant(msr, val, type);
#endif

	return __native_rdmsr_variable(msr, val, type);
}

static __always_inline u64 native_rdmsrq(u32 msr)
{
	u64 val = 0;

	__native_rdmsr(msr, &val, EX_TYPE_RDMSR);
	return val;
}

#define native_rdmsr(msr, low, high)			\
do {							\
	u64 __val = native_rdmsrq(msr);			\
	(void)((low) = (u32)__val);			\
	(void)((high) = (u32)(__val >> 32));		\
} while (0)

static inline u64 native_read_msr(u32 msr)
{
	u64 val = native_rdmsrq(msr);

	if (tracepoint_enabled(read_msr))
		do_trace_read_msr(msr, val, 0);

	return val;
}

static inline u64 native_read_msr_safe(u32 msr, int *err)
{
	u64 val = 0;

	*err = __native_rdmsr(msr, &val, EX_TYPE_RDMSR_SAFE) ? -EIO : 0;

	if (tracepoint_enabled(read_msr))
		do_trace_read_msr(msr, val, *err);

	return val;
}

#ifdef CONFIG_XEN_PV
/* No plan to support immediate form MSR instructions in Xen */
static __always_inline bool __xenpv_rdmsr(u32 msr, u64 *val, int type)
{
	asm_inline volatile goto(
		"1: call asm_xen_read_msr\n\t"
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For CALL */

		: [val] "=a" (*val), ASM_CALL_CONSTRAINT
		: "c" (msr), [type] "i" (type)
		: "rdx"
		: badmsr);

	return false;

badmsr:
	return true;
}
#endif

static __always_inline bool __rdmsr(u32 msr, u64 *val, int type)
{
	/* When built with CONFIG_XEN_PV and running on Xen hypervisor. */
	if (cpu_feature_enabled(X86_FEATURE_XENPV)) {
		const enum pv_msr_action action = get_pv_msr_action(msr);

		if (action == PV_MSR_IGNORE)
			return false;
		else
			return __xenpv_rdmsr(msr, val, type);
	} else {
		/*
		 * 1) When built with !CONFIG_XEN_PV.
		 * 2) When built with CONFIG_XEN_PV but not running on Xen hypervisor.
		 */
		bool ret = __native_rdmsr(msr, val, type);

		if (tracepoint_enabled(read_msr))
			do_trace_read_msr(msr, *val, ret ? -EIO : 0);

		return ret;
	}
}

#define rdmsrq(msr, val)				\
do {							\
	u64 ___val = 0;					\
	__rdmsr((msr), &___val, EX_TYPE_RDMSR);		\
	(val) = ___val;					\
} while (0)

#define rdmsr(msr, low, high)				\
do {							\
	u64 __val = 0;					\
	rdmsrq((msr), __val);				\
	(void)((low) = (u32)__val);			\
	(void)((high) = (u32)(__val >> 32));		\
} while (0)

static __always_inline int rdmsrq_safe(u32 msr, u64 *val)
{
	return __rdmsr(msr, val, EX_TYPE_RDMSR_SAFE) ? -EIO : 0;
}

#define rdmsr_safe(msr, low, high)			\
({							\
	u64 __val = 0;					\
	int __err = rdmsrq_safe((msr), &__val);		\
	(*low) = (u32)__val;				\
	(*high) = (u32)(__val >> 32);			\
	__err;						\
})

/*
 * Below is a diagram illustrating the derivation of the MSR write APIs:
 *
 *      __native_wrmsr_variable()    __native_wrmsr_constant()
 *                         \           /
 *                          \         /
 *                         __native_wrmsr()   ------------------------
 *                            /     \                                |
 *                           /       \                               |
 *        native_wrmsrq_no_trace()    native_write_msr_safe()        |
 *                   /        \                                      |
 *                  /          \                                     |
 * native_wrmsr_no_trace()    native_wrmsrq()                        |
 *                                                                   |
 *                                                                   |
 *                                                                   |
 *                   __xenpv_wrmsr()                                 |
 *                         |                                         |
 *                         |                                         |
 *                      __wrmsr()   <---------------------------------
 *                       /    \
 *                      /      \
 *                 wrmsrq()   wrmsrq_safe()
 *                    /          \
 *                   /            \
 *                wrmsr()        wrmsr_safe()
 */

/*
 * Non-serializing WRMSR, when available.
 *
 * Otherwise, it falls back to a serializing WRMSR.
 */
static __always_inline bool __native_wrmsr_variable(u32 msr, u64 val, int type)
{
#ifdef CONFIG_X86_64
	BUILD_BUG_ON(__builtin_constant_p(msr));
#endif

	/*
	 * WRMSR is 2 bytes.  WRMSRNS is 3 bytes.  Pad WRMSR with a redundant
	 * DS prefix to avoid a trailing NOP.
	 */
	asm_inline volatile goto(
		"1:\n"
		ALTERNATIVE("ds wrmsr",
			    ASM_WRMSRNS,
			    X86_FEATURE_WRMSRNS)
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])

		:
		: "c" (msr), "a" ((u32)val), "d" ((u32)(val >> 32)), [type] "i" (type)
		: "memory"
		: badmsr);

	return false;

badmsr:
	return true;
}

#ifdef CONFIG_X86_64
/*
 * Non-serializing WRMSR or its immediate form, when available.
 *
 * Otherwise, it falls back to a serializing WRMSR.
 */
static __always_inline bool __native_wrmsr_constant(u32 msr, u64 val, int type)
{
	BUILD_BUG_ON(!__builtin_constant_p(msr));

	asm_inline volatile goto(
		"1:\n"
		ALTERNATIVE_2(PREPARE_RCX_RDX_FOR_WRMSR
			      "2: ds wrmsr",
			      PREPARE_RCX_RDX_FOR_WRMSR
			      ASM_WRMSRNS,
			      X86_FEATURE_WRMSRNS,
			      ASM_WRMSRNS_IMM,
			      X86_FEATURE_MSR_IMM)
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For WRMSRNS immediate */
		_ASM_EXTABLE_TYPE(2b, %l[badmsr], %c[type])	/* For WRMSR(NS) */

		:
		: [val] "a" (val), [msr] "i" (msr), [type] "i" (type)
		: "memory", "ecx", "rdx"
		: badmsr);

	return false;

badmsr:
	return true;
}
#endif

static __always_inline bool __native_wrmsr(u32 msr, u64 val, int type)
{
#ifdef CONFIG_X86_64
	if (__builtin_constant_p(msr))
		return __native_wrmsr_constant(msr, val, type);
#endif

	return __native_wrmsr_variable(msr, val, type);
}

static __always_inline void native_wrmsrq_no_trace(u32 msr, u64 val)
{
	__native_wrmsr(msr, val, EX_TYPE_WRMSR);
}

static __always_inline void native_wrmsr_no_trace(u32 msr, u32 low, u32 high)
{
	native_wrmsrq_no_trace(msr, (u64)high << 32 | low);
}

static inline void notrace native_wrmsrq(u32 msr, u64 val)
{
	native_wrmsrq_no_trace(msr, val);

	if (tracepoint_enabled(write_msr))
		do_trace_write_msr(msr, val, 0);
}

static inline int notrace native_write_msr_safe(u32 msr, u64 val)
{
	int err = __native_wrmsr(msr, val, EX_TYPE_WRMSR_SAFE) ? -EIO : 0;

	if (tracepoint_enabled(write_msr))
		do_trace_write_msr(msr, val, err);

	return err;
}

#ifdef CONFIG_XEN_PV
/* No plan to support immediate form MSR instructions in Xen */
static __always_inline bool __xenpv_wrmsr(u32 msr, u64 val, int type)
{
	asm_inline volatile goto(
		"call asm_xen_write_msr\n\t"
		"jnz 2f\n\t"
		ALTERNATIVE("1: ds wrmsr",
			    ASM_WRMSRNS,
			    X86_FEATURE_WRMSRNS)
		"2:\n"
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For WRMSR(NS) */

		: ASM_CALL_CONSTRAINT
		: "a" (val), "c" (msr), [type] "i" (type)
		: "rdx"
		: badmsr);

	return false;

badmsr:
	return true;
}
#endif

static __always_inline bool __wrmsr(u32 msr, u64 val, int type)
{
	/* When built with CONFIG_XEN_PV and running on Xen hypervisor. */
	if (cpu_feature_enabled(X86_FEATURE_XENPV)) {
		const enum pv_msr_action action = get_pv_msr_action(msr);

		if (action == PV_MSR_IGNORE)
			return false;
		else
			return __xenpv_wrmsr(msr, val, type);
	} else {
		/*
		 * 1) When built with !CONFIG_XEN_PV.
		 * 2) When built with CONFIG_XEN_PV but not running on Xen hypervisor.
		 */
		bool ret = __native_wrmsr(msr, val, type);

		if (tracepoint_enabled(write_msr))
			do_trace_write_msr(msr, val, ret ? -EIO : 0);

		return ret;
	}
}

static __always_inline void wrmsrq(u32 msr, u64 val)
{
	__wrmsr(msr, val, EX_TYPE_WRMSR);
}

static __always_inline void wrmsr(u32 msr, u32 low, u32 high)
{
	wrmsrq(msr, (u64)high << 32 | low);
}

static __always_inline int wrmsrq_safe(u32 msr, u64 val)
{
	return __wrmsr(msr, val, EX_TYPE_WRMSR_SAFE) ? -EIO : 0;
}

static __always_inline int wrmsr_safe(u32 msr, u32 low, u32 high)
{
	return wrmsrq_safe(msr, (u64)high << 32 | low);
}

extern int rdmsr_safe_regs(u32 regs[8]);
extern int wrmsr_safe_regs(u32 regs[8]);

static __always_inline u64 rdpmcq(int counter)
{
	DECLARE_ARGS(val, low, high);

	/* When built with CONFIG_XEN_PV and running on Xen hypervisor. */
	if (cpu_feature_enabled(X86_FEATURE_XENPV))
		return xen_read_pmc(counter);

	/*
	 * 1) When built with !CONFIG_XEN_PV.
	 * 2) When built with CONFIG_XEN_PV but not running on Xen hypervisor.
	 */
	asm_inline volatile("rdpmc" : EAX_EDX_RET(val, low, high) : "c" (counter));

	if (tracepoint_enabled(rdpmc))
		do_trace_rdpmc(counter, EAX_EDX_VAL(val, low, high), 0);

	return EAX_EDX_VAL(val, low, high);
}

struct msr __percpu *msrs_alloc(void);
void msrs_free(struct msr __percpu *msrs);
int msr_set_bit(u32 msr, u8 bit);
int msr_clear_bit(u32 msr, u8 bit);

#ifdef CONFIG_SMP
int rdmsr_on_cpu(unsigned int cpu, u32 msr_no, u32 *l, u32 *h);
int wrmsr_on_cpu(unsigned int cpu, u32 msr_no, u32 l, u32 h);
int rdmsrq_on_cpu(unsigned int cpu, u32 msr_no, u64 *q);
int wrmsrq_on_cpu(unsigned int cpu, u32 msr_no, u64 q);
void rdmsr_on_cpus(const struct cpumask *mask, u32 msr_no, struct msr __percpu *msrs);
void wrmsr_on_cpus(const struct cpumask *mask, u32 msr_no, struct msr __percpu *msrs);
int rdmsr_safe_on_cpu(unsigned int cpu, u32 msr_no, u32 *l, u32 *h);
int wrmsr_safe_on_cpu(unsigned int cpu, u32 msr_no, u32 l, u32 h);
int rdmsrq_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 *q);
int wrmsrq_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 q);
int rdmsr_safe_regs_on_cpu(unsigned int cpu, u32 regs[8]);
int wrmsr_safe_regs_on_cpu(unsigned int cpu, u32 regs[8]);
#else  /*  CONFIG_SMP  */
static inline int rdmsr_on_cpu(unsigned int cpu, u32 msr_no, u32 *l, u32 *h)
{
	rdmsr(msr_no, *l, *h);
	return 0;
}
static inline int wrmsr_on_cpu(unsigned int cpu, u32 msr_no, u32 l, u32 h)
{
	wrmsr(msr_no, l, h);
	return 0;
}
static inline int rdmsrq_on_cpu(unsigned int cpu, u32 msr_no, u64 *q)
{
	rdmsrq(msr_no, *q);
	return 0;
}
static inline int wrmsrq_on_cpu(unsigned int cpu, u32 msr_no, u64 q)
{
	wrmsrq(msr_no, q);
	return 0;
}
static inline void rdmsr_on_cpus(const struct cpumask *m, u32 msr_no,
				struct msr __percpu *msrs)
{
	rdmsr_on_cpu(0, msr_no, raw_cpu_ptr(&msrs->l), raw_cpu_ptr(&msrs->h));
}
static inline void wrmsr_on_cpus(const struct cpumask *m, u32 msr_no,
				struct msr __percpu *msrs)
{
	wrmsr_on_cpu(0, msr_no, raw_cpu_read(msrs->l), raw_cpu_read(msrs->h));
}
static inline int rdmsr_safe_on_cpu(unsigned int cpu, u32 msr_no,
				    u32 *l, u32 *h)
{
	return rdmsr_safe(msr_no, l, h);
}
static inline int wrmsr_safe_on_cpu(unsigned int cpu, u32 msr_no, u32 l, u32 h)
{
	return wrmsr_safe(msr_no, l, h);
}
static inline int rdmsrq_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 *q)
{
	return rdmsrq_safe(msr_no, q);
}
static inline int wrmsrq_safe_on_cpu(unsigned int cpu, u32 msr_no, u64 q)
{
	return wrmsrq_safe(msr_no, q);
}
static inline int rdmsr_safe_regs_on_cpu(unsigned int cpu, u32 regs[8])
{
	return rdmsr_safe_regs(regs);
}
static inline int wrmsr_safe_regs_on_cpu(unsigned int cpu, u32 regs[8])
{
	return wrmsr_safe_regs(regs);
}
#endif  /* CONFIG_SMP */

/* Compatibility wrappers: */
#define rdmsrl(msr, val) rdmsrq(msr, val)
#define wrmsrl(msr, val) wrmsrq(msr, val)

#endif /* __ASSEMBLER__ */
#endif /* _ASM_X86_MSR_H */
