/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MSR_H
#define _ASM_X86_MSR_H

#include "msr-index.h"

#ifndef __ASSEMBLER__

#include <asm/asm.h>
#include <asm/errno.h>
#include <asm/cpufeature.h>
#include <asm/cpumask.h>
#include <asm/msr-xen.h>
#include <asm/shared/msr.h>
#include <uapi/asm/msr.h>

#include <linux/types.h>
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

/*
 * Called only from an MSR fault handler, the instruction pointer points to
 * the MSR access instruction that caused the fault.
 */
static __always_inline bool is_msr_imm_insn(void *ip)
{
	/*
	 * A full decoder for immediate form MSR instructions appears excessive.
	 */
#ifdef CONFIG_X86_64
	const u8 msr_imm_insn_prefix[] = { 0xc4, 0xe7 };

	return !memcmp(ip, msr_imm_insn_prefix, sizeof(msr_imm_insn_prefix));
#else
	return false;
#endif
}

/*
 * There are two sets of APIs for MSR accesses: native APIs and generic APIs.
 * Native MSR APIs execute MSR instructions directly, regardless of whether the
 * CPU is paravirtualized or native.  Generic MSR APIs determine the appropriate
 * MSR access method at runtime, allowing them to be used generically on both
 * paravirtualized and native CPUs.
 *
 * When the compiler can determine the MSR number at compile time, the APIs
 * with the suffix _constant() are used to enable the immediate form MSR
 * instructions when available.  The APIs with the suffix _variable() are
 * used when the MSR number is not known until run time.
 *
 * Below is a diagram illustrating the derivation of the MSR read APIs:
 *
 *      __native_rdmsrq_variable()    __native_rdmsrq_constant()
 *                         \           /
 *                          \         /
 *                         __native_rdmsrq()   -----------------------
 *                            /     \                                |
 *                           /       \                               |
 *        native_rdmsrq_no_trace()    native_read_msr_safe()         |
 *                   /      \                                        |
 *                  /        \                                       |
 * native_rdmsr_no_trace()    native_rdmsrq()                        |
 *                                                                   |
 *                                                                   |
 *                                                                   |
 *                    __xenpv_rdmsrq()                               |
 *                         |                                         |
 *                         |                                         |
 *                      __rdmsrq()   <--------------------------------
 *                       /    \
 *                      /      \
 *                 rdmsrq()   rdmsrq_safe()
 *                    /          \
 *                   /            \
 *                rdmsr()        rdmsr_safe()
 */

static __always_inline bool __native_rdmsrq_variable(u32 msr, u64 *val, int type)
{
#ifdef CONFIG_X86_64
	BUILD_BUG_ON(__builtin_constant_p(msr));

	asm_inline volatile goto(
		"1:\n"
		RDMSR_AND_SAVE_RESULT
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For RDMSR */

		: [val] "=a" (*val)
		: "c" (msr), [type] "i" (type)
		: "rdx"
		: badmsr);
#else
	asm_inline volatile goto(
		"1: rdmsr\n\t"
		_ASM_EXTABLE_TYPE(1b, %l[badmsr], %c[type])	/* For RDMSR */

		: "=A" (*val)
		: "c" (msr), [type] "i" (type)
		:
		: badmsr);
#endif

	return false;

badmsr:
	*val = 0;

	return true;
}

#ifdef CONFIG_X86_64
static __always_inline bool __native_rdmsrq_constant(u32 msr, u64 *val, int type)
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
		: "ecx", "rdx"
		: badmsr);

	return false;

badmsr:
	*val = 0;

	return true;
}
#endif

static __always_inline bool __native_rdmsrq(u32 msr, u64 *val, int type)
{
#ifdef CONFIG_X86_64
	if (__builtin_constant_p(msr))
		return __native_rdmsrq_constant(msr, val, type);
#endif

	return __native_rdmsrq_variable(msr, val, type);
}

static __always_inline u64 native_rdmsrq_no_trace(u32 msr)
{
	u64 val = 0;

	__native_rdmsrq(msr, &val, EX_TYPE_RDMSR);
	return val;
}

#define native_rdmsr_no_trace(msr, low, high)		\
do {							\
	u64 __val = native_rdmsrq_no_trace(msr);	\
	(void)((low) = (u32)__val);			\
	(void)((high) = (u32)(__val >> 32));		\
} while (0)

static inline u64 native_rdmsrq(u32 msr)
{
	u64 val = native_rdmsrq_no_trace(msr);

	if (tracepoint_enabled(read_msr))
		do_trace_read_msr(msr, val, 0);

	return val;
}

static inline int native_read_msr_safe(u32 msr, u64 *val)
{
	int err;

	err = __native_rdmsrq(msr, val, EX_TYPE_RDMSR_SAFE) ? -EIO : 0;

	if (tracepoint_enabled(read_msr))
		do_trace_read_msr(msr, *val, err);

	return err;
}

static __always_inline bool __rdmsrq(u32 msr, u64 *val, int type)
{
	bool ret;

	if (cpu_feature_enabled(X86_FEATURE_XENPV))
		return __xenpv_rdmsrq(msr, val, type);

	ret = __native_rdmsrq(msr, val, type);

	if (tracepoint_enabled(read_msr))
		do_trace_read_msr(msr, *val, ret ? -EIO : 0);

	return ret;
}

#define rdmsrq(msr, val)				\
do {							\
	u64 ___val = 0;					\
	__rdmsrq((msr), &___val, EX_TYPE_RDMSR);	\
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
	return __rdmsrq(msr, val, EX_TYPE_RDMSR_SAFE) ? -EIO : 0;
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
 *      __native_wrmsrq_variable()    __native_wrmsrq_constant()
 *                         \           /
 *                          \         /
 *                         __native_wrmsrq()   -----------------------
 *                            /     \                                |
 *                           /       \                               |
 *        native_wrmsrq_no_trace()    native_wrmsrq_safe()           |
 *                   /        \                                      |
 *                  /          \                                     |
 * native_wrmsr_no_trace()    native_wrmsrq()                        |
 *                                                                   |
 *                                                                   |
 *                                                                   |
 *                   __xenpv_wrmsrq()                                |
 *                         |                                         |
 *                         |                                         |
 *                      __wrmsrq()   <--------------------------------
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
static __always_inline bool __native_wrmsrq_variable(u32 msr, u64 val, int type)
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
static __always_inline bool __native_wrmsrq_constant(u32 msr, u64 val, int type)
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

static __always_inline bool __native_wrmsrq(u32 msr, u64 val, int type)
{
#ifdef CONFIG_X86_64
	if (__builtin_constant_p(msr))
		return __native_wrmsrq_constant(msr, val, type);
#endif

	return __native_wrmsrq_variable(msr, val, type);
}

static __always_inline void native_wrmsrq_no_trace(u32 msr, u64 val)
{
	__native_wrmsrq(msr, val, EX_TYPE_WRMSR);
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

static inline int notrace native_wrmsrq_safe(u32 msr, u64 val)
{
	int err = __native_wrmsrq(msr, val, EX_TYPE_WRMSR_SAFE) ? -EIO : 0;

	if (tracepoint_enabled(write_msr))
		do_trace_write_msr(msr, val, err);

	return err;
}

static __always_inline bool __wrmsrq(u32 msr, u64 val, int type)
{
	bool ret;

	if (cpu_feature_enabled(X86_FEATURE_XENPV))
		return __xenpv_wrmsrq(msr, val, type);

	ret = __native_wrmsrq(msr, val, type);

	if (tracepoint_enabled(write_msr))
		do_trace_write_msr(msr, val, ret ? -EIO : 0);

	return ret;
}

static __always_inline void wrmsrq(u32 msr, u64 val)
{
	__wrmsrq(msr, val, EX_TYPE_WRMSR);
}

static __always_inline void wrmsr(u32 msr, u32 low, u32 high)
{
	wrmsrq(msr, (u64)high << 32 | low);
}

static __always_inline int wrmsrq_safe(u32 msr, u64 val)
{
	return __wrmsrq(msr, val, EX_TYPE_WRMSR_SAFE) ? -EIO : 0;
}

static __always_inline int wrmsr_safe(u32 msr, u32 low, u32 high)
{
	return wrmsrq_safe(msr, (u64)high << 32 | low);
}

extern int rdmsr_safe_regs(u32 regs[8]);
extern int wrmsr_safe_regs(u32 regs[8]);

static __always_inline u64 native_rdpmc(int counter)
{
	EAX_EDX_DECLARE_ARGS(val, low, high);

	asm_inline volatile("rdpmc" : EAX_EDX_RET(val, low, high) : "c" (counter));

	if (tracepoint_enabled(rdpmc))
		do_trace_rdpmc(counter, EAX_EDX_VAL(val, low, high), 0);

	return EAX_EDX_VAL(val, low, high);
}

static __always_inline u64 rdpmc(int counter)
{
	if (!cpu_feature_enabled(X86_FEATURE_XENPV))
		return native_rdpmc(counter);

	return xen_rdpmc(counter);
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
#define rdmsrl_on_cpu(cpu, msr, q) rdmsrq_on_cpu(cpu, msr, q)

#endif /* __ASSEMBLER__ */
#endif /* _ASM_X86_MSR_H */
