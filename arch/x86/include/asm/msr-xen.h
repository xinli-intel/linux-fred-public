/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_MSR_XEN_H
#define _ASM_X86_MSR_XEN_H

#ifndef __ASSEMBLER__

#ifdef CONFIG_XEN_PV
#include <linux/types.h>
#include <asm/cpufeature.h>

extern u64 xen_read_pmc(int counter);

static __always_inline u64 xen_rdpmc(int counter)
{
	BUG_ON(!cpu_feature_enabled(X86_FEATURE_XENPV));

	return xen_read_pmc(counter);
}
#else /* !CONFIG_XEN_PV */
static __always_inline u64 xen_rdpmc(int counter) { return 0; }
#endif /* CONFIG_XEN_PV */

#endif /* __ASSEMBLER__ */
#endif /* _ASM_X86_MSR_XEN_H */
