// SPDX-License-Identifier: GPL-2.0-only
/*
 * FRED nested exception tests
 *
 * Copyright (C) 2023, Intel, Inc.
 */
#define _GNU_SOURCE /* for program_invocation_short_name */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <asm/msr-index.h>

#include "apic.h"
#include "kvm_util.h"
#include "test_util.h"
#include "guest_modes.h"
#include "processor.h"

#define IRQ_VECTOR 0xAA

#define FRED_STKLVL(v,l)		(_AT(unsigned long, l) << (2 * (v)))
#define FRED_CONFIG_ENTRYPOINT(p)	_AT(unsigned long, (p))

/* This address is already mapped in guest page table. */
#define FRED_VALID_RSP			0x8000

/*
 * The following addresses are not yet mapped in both EPT and guest page
 * tables at the beginning.  As a result, it causes an EPT violation VM
 * exit with an original guest #PF to access any of them for the first
 * time.
 *
 * Use these addresses as guest FRED RSP0 to generate nested #PFs to test
 * if event data are properly virtualized.
 */
static unsigned long fred_invalid_rsp[4] = {
	0x0,
	0xf0000000,
	0xe0000000,
	0xd0000000,
};

extern char asm_user_nop[];
extern char asm_user_ud[];
extern char asm_done_fault[];

extern void asm_test_fault(int test);

/*
 * user level code for triggering faults.
 */
asm(".pushsection .text\n"
    ".align 4096\n"

    ".type asm_user_nop, @function\n"
    "asm_user_nop:\n"
    "1: .byte 0x90\n"
    "jmp 1b\n"

    ".fill asm_user_ud - ., 1, 0xcc\n"

    ".type asm_user_ud, @function\n"
    ".org asm_user_nop + 16\n"
    "asm_user_ud:\n"
    /* Trigger a #UD */
    "ud2\n"

    ".align 4096, 0xcc\n"
    ".popsection");

/* Send current stack level and #PF address */
#define GUEST_SYNC_CSL_FA(__stage, __pf_address)		\
	GUEST_SYNC_ARGS(__stage, __pf_address, 0, 0, 0)

void fred_entry_from_user(struct fred_stack *stack)
{
	u32 current_stack_level = rdmsr(MSR_IA32_FRED_CONFIG) & 0x3;

	GUEST_SYNC_CSL_FA(current_stack_level, stack->event_data);

	/* Do NOT go back to user level, continue the next test instead */
	stack->ssx = 0x18;
	stack->csx = 0x10;
	stack->ip = (u64)&asm_done_fault;
}

void fred_entry_from_kernel(struct fred_stack *stack)
{
	/*
	 * Keep NMI blocked to delay the delivery of the next NMI until
	 * returning to user level.
	 * */
	stack->ssx &= ~FRED_SSX_NMI;
}

#define PUSH_REGS	\
	"push %rdi\n"	\
	"push %rsi\n"	\
	"push %rdx\n"	\
	"push %rcx\n"	\
	"push %rax\n"	\
	"push %r8\n"	\
	"push %r9\n"	\
	"push %r10\n"	\
	"push %r11\n"	\
	"push %rbx\n"	\
	"push %rbp\n"	\
	"push %r12\n"	\
	"push %r13\n"	\
	"push %r14\n"	\
	"push %r15\n"

#define POP_REGS	\
	"pop %r15\n"	\
	"pop %r14\n"	\
	"pop %r13\n"	\
	"pop %r12\n"	\
	"pop %rbp\n"	\
	"pop %rbx\n"	\
	"pop %r11\n"	\
	"pop %r10\n"	\
	"pop %r9\n"	\
	"pop %r8\n"	\
	"pop %rax\n"	\
	"pop %rcx\n"	\
	"pop %rdx\n"	\
	"pop %rsi\n"	\
	"pop %rdi\n"

/*
 * FRED entry points.
 */
asm(".pushsection .text\n"
    ".type asm_fred_entrypoint_user, @function\n"
    ".align 4096\n"
    "asm_fred_entrypoint_user:\n"
    "endbr64\n"
    PUSH_REGS
    "movq %rsp, %rdi\n"
    "call fred_entry_from_user\n"
    POP_REGS
    /* Do NOT go back to user level, continue the next test instead */
    ".byte 0xf2,0x0f,0x01,0xca\n"	/* ERETS */

    ".fill asm_fred_entrypoint_kernel - ., 1, 0xcc\n"

    ".type asm_fred_entrypoint_kernel, @function\n"
    ".org asm_fred_entrypoint_user + 256\n"
    "asm_fred_entrypoint_kernel:\n"
    "endbr64\n"
    PUSH_REGS
    "movq %rsp, %rdi\n"
    "call fred_entry_from_kernel\n"
    POP_REGS
    ".byte 0xf2,0x0f,0x01,0xca\n"	/* ERETS */
    ".align 4096, 0xcc\n"
    ".popsection");

extern char asm_fred_entrypoint_user[];

/*
 * Prepare a FRED stack frame for ERETU to return to user level code,
 * nop or ud2.
 *
 * Because FRED RSP0 is deliberately not mapped in guest page table,
 * the delivery of interrupt/NMI or #UD from ring 3 causes a nested
 * #PF, which is then delivered on FRED RSPx (x is 1, 2 or 3,
 * determinated by MSR FRED_STKLVL[PF_VECTOR]).
 */
asm(".pushsection .text\n"
    ".type asm_test_fault, @function\n"
    ".align 4096\n"
    "asm_test_fault:\n"
    "endbr64\n"
    "push %rbp\n"
    "mov %rsp, %rbp\n"
    "and $(~0x3f), %rsp\n"
    "push $0\n"
    "push $0\n"
    "mov $0x2b, %rax\n"
    /* Unblock NMI */
    "bts $18, %rax\n"
    /* Set long mode bit */
    "bts $57, %rax\n"
    "push %rax\n"
    /* No stack required for the FRED user level test code */
    "push $0\n"
    "pushf\n"
    "pop %rax\n"
    /* Allow external interrupts */
    "bts $9, %rax\n"
    "push %rax\n"
    "mov $0x33, %rax\n"
    "push %rax\n"
    "cmp $0, %edi\n"
    "jne 1f\n"
    "lea asm_user_nop(%rip), %rax\n"
    "jmp 2f\n"
    "1: lea asm_user_ud(%rip), %rax\n"
    "2: push %rax\n"
    "push $0\n"
    /* ERETU to user level code to allow event delivery immediately */
    ".byte 0xf3,0x0f,0x01,0xca\n"
    "asm_done_fault:\n"
    "mov %rbp, %rsp\n"
    "pop %rbp\n"
    "ret\n"
    ".align 4096, 0xcc\n"
    ".popsection");

/*
 * To fully test the underlying FRED VMX code, this test should be run one
 * more round with EPT disabled to inject page faults as nested exceptions.
 */
static void guest_code(void)
{
	wrmsr(MSR_IA32_FRED_CONFIG,
	      FRED_CONFIG_ENTRYPOINT(asm_fred_entrypoint_user));

	wrmsr(MSR_IA32_FRED_RSP1, FRED_VALID_RSP);
	wrmsr(MSR_IA32_FRED_RSP2, FRED_VALID_RSP);
	wrmsr(MSR_IA32_FRED_RSP3, FRED_VALID_RSP);

	/* Enable FRED */
	set_cr4(get_cr4() | X86_CR4_FRED);

	x2apic_enable();

	wrmsr(MSR_IA32_FRED_STKLVLS, FRED_STKLVL(PF_VECTOR, 1));
	wrmsr(MSR_IA32_FRED_RSP0, fred_invalid_rsp[1]);
	/* 1: ud2 to generate #UD */
	asm_test_fault(1);

	wrmsr(MSR_IA32_FRED_STKLVLS, FRED_STKLVL(PF_VECTOR, 2));
	wrmsr(MSR_IA32_FRED_RSP0, fred_invalid_rsp[2]);
	asm volatile("cli");
	/* Create a pending interrupt on current vCPU */
	x2apic_write_reg(APIC_ICR, APIC_DEST_SELF | APIC_INT_ASSERT |
			 APIC_DM_FIXED | IRQ_VECTOR);
	/* Return to ring 3 */
	asm_test_fault(0);
	x2apic_write_reg(APIC_EOI, 0);

	wrmsr(MSR_IA32_FRED_STKLVLS, FRED_STKLVL(PF_VECTOR, 3));
	wrmsr(MSR_IA32_FRED_RSP0, fred_invalid_rsp[3]);
	/*
	 * The first NMI is just to have NMI blocked in ring 0, because
	 * fred_entry_from_kernel() deliberately clears the NMI bit in
	 * FRED stack frame.
	 */
	x2apic_write_reg(APIC_ICR, APIC_DEST_SELF | APIC_INT_ASSERT |
			 APIC_DM_NMI | NMI_VECTOR);
	/* The second NMI will be delivered after returning to ring 3 */
	x2apic_write_reg(APIC_ICR, APIC_DEST_SELF | APIC_INT_ASSERT |
			 APIC_DM_NMI | NMI_VECTOR);
	/* Return to ring 3 */
	asm_test_fault(0);

	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	struct ucall uc;
	uint64_t expected_current_stack_level = 1;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_FRED));

	vm = __vm_create_with_vcpus(VM_MODE_PXXV48_4K_USER, 1, 0,
				    guest_code, &vcpu);

	while (true) {
		uint64_t r;

		vcpu_run(vcpu);

		r = get_ucall(vcpu, &uc);

		if (r == UCALL_DONE)
			break;

		if (r == UCALL_SYNC) {
			TEST_ASSERT((uc.args[1] == expected_current_stack_level) &&
				    (uc.args[2] == fred_invalid_rsp[expected_current_stack_level] - 1),
				    "Incorrect stack level %lx and #PF address %lx\n",
				    uc.args[1], uc.args[2]);
			expected_current_stack_level++;
		}
	}

	kvm_vm_free(vm);
	return 0;
}
