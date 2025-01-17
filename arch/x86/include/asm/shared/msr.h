/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHARED_MSR_H
#define _ASM_X86_SHARED_MSR_H

struct msr {
	union {
		struct {
			u32 l;
			u32 h;
		};
		u64 q;
	};
};

/* The GNU Assembler (Gas) with Binutils 2.40 adds WRMSRNS support */
#if defined(CONFIG_AS_IS_GNU) && CONFIG_AS_VERSION >= 24000
#define ASM_WRMSRNS		"wrmsrns"
#else
#define ASM_WRMSRNS		_ASM_BYTES(0x0f,0x01,0xc6)
#endif

/* The GNU Assembler (Gas) with Binutils 2.41 adds the .insn directive support */
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

#endif /* _ASM_X86_SHARED_MSR_H */
