/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MSR_XEN_H
#define _ASM_X86_MSR_XEN_H

#ifndef __ASSEMBLER__

#ifdef CONFIG_XEN_PV
#include <linux/types.h>
#include <asm/asm.h>
#include <asm/cpufeature.h>
#include <asm/shared/msr.h>

extern void asm_xen_write_msr(void);
extern u64 xen_read_pmc(int counter);

/* No plan to support immediate form MSR instructions in Xen */
static __always_inline bool __xenpv_wrmsrq(u32 msr, u64 val, int type)
{
	BUG_ON(!cpu_feature_enabled(X86_FEATURE_XENPV));

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
		: "memory", "rdx"
		: badmsr);

	return false;

badmsr:
	return true;
}

static __always_inline u64 xen_rdpmc(int counter)
{
	BUG_ON(!cpu_feature_enabled(X86_FEATURE_XENPV));

	return xen_read_pmc(counter);
}
#else /* !CONFIG_XEN_PV */
static __always_inline bool __xenpv_wrmsrq(u32 msr, u64 val, int type) { return false; }
static __always_inline u64 xen_rdpmc(int counter) { return 0; }
#endif /* CONFIG_XEN_PV */

#endif /* __ASSEMBLER__ */
#endif /* _ASM_X86_MSR_XEN_H */
