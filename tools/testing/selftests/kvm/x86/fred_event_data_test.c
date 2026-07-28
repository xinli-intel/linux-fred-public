// SPDX-License-Identifier: GPL-2.0-only
/*
 * FRED exception event data tests
 *
 * Copyright (C) 2026, Intel, Inc.
 */
#define _GNU_SOURCE /* for program_invocation_short_name */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <asm/msr-index.h>

#include "kvm_util.h"
#include "test_util.h"
#include "processor.h"

#define FRED_STKLVL(v,l)		(_AT(unsigned long, l) << (2 * (v)))
#define FRED_CONFIG_ENTRYPOINT(p)	_AT(unsigned long, (p))

/* This address is already mapped in guest page table. */
#define FRED_VALID_RSP		0x8000

/*
 * Bad Stack: only the guest page-table mapping is provided; the
 * memslot is intentionally omitted, so there is no EPT mapping.
 * A frame push to this "bad" stack will trigger an EPT violation, and
 * because that push happens during event vectoring, KVM cannot emulate
 * it (kvm_can_emulate_event_vectoring) and exits to userspace as
 * "KVM_INTERNAL_ERROR_DELIVERY_EV" instead of handling the fault
 * itself.
 */
#define BAD_STACK_GPA		0xc0000000ul
#define BAD_STACK_SLOT		6
#define BAD_STACK_NPAGES	16
/* FRED stacks grow down; point the level-2 RSP at the top of the region. */
#define BAD_STACK_TOP		(BAD_STACK_GPA + BAD_STACK_NPAGES * 0x1000)

/* No guest-PT mapping; to trigger non-nested #PF. */
#define FRED_PF_GVA			0xe0000000ul
/* No guest-PT mapping; used for #UD's RSP to trigger nested #PF. */
#define UNMAPPED_UD_RSP		0xf0000000ul

static bool guest_is_nested;
extern char asm_fred_entrypoint[];

void fred_entry_from_kernel(struct fred_stack *stack)
{
	/*
	 * Fetch event_data (after migration) pushed in the FRED stack frame
	 * and report back to the host to verify.
	 */
	GUEST_SYNC2(stack->event_data, !!(stack->ssx & FRED_SSX_NESTED));
}

asm(".pushsection .text\n"
    ".type asm_fred_entrypoint, @function\n"
    ".align 4096\n"
    "asm_fred_entrypoint:\n"
    "ud2\n" /* User-level entry: unreachable in ring-0-only test. */

    ".org asm_fred_entrypoint + 256, 0xcc\n"
    PUSH_REGS
    "movq %rsp, %rdi\n"
    "call fred_entry_from_kernel\n"
    POP_REGS
    ".byte 0xf2,0x0f,0x01,0xca\n"	/* ERETS */
    ".align 4096, 0xcc\n"
    ".popsection");

static void guest_code(void)
{
	wrmsr(MSR_IA32_FRED_CONFIG,
	      FRED_CONFIG_ENTRYPOINT(asm_fred_entrypoint));

	/*
	 * Ensure #PF goes to higher level than #UD, so that nested #PF can
	 * be pushed to BAD STACK as expected.
	 */
	wrmsr(MSR_IA32_FRED_STKLVLS,
	      FRED_STKLVL(PF_VECTOR, 2) | FRED_STKLVL(UD_VECTOR, 1));

	wrmsr(MSR_IA32_FRED_RSP1, UNMAPPED_UD_RSP);
	wrmsr(MSR_IA32_FRED_RSP2, BAD_STACK_TOP);
	wrmsr(MSR_IA32_FRED_RSP3, FRED_VALID_RSP);

	set_cr4(get_cr4() | X86_CR4_FRED);

	/* Hand control back so the host can select the scenario. */
	GUEST_SYNC(true);

	if (guest_is_nested)
		asm volatile("ud2");
	else
		*(volatile u64 *)FRED_PF_GVA;
}

static void run_until_true(struct kvm_vcpu *vcpu)
{
	struct ucall uc;

	vcpu_run(vcpu);
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);
	TEST_ASSERT(get_ucall(vcpu, &uc) == UCALL_SYNC && uc.args[1] == true,
				"Guest failed to reach the FRED-ready sync point");
}

static u64 inject_and_freeze(struct kvm_vm *vm, struct kvm_vcpu *vcpu,
			     bool is_nested)
{
	struct kvm_vcpu_events events;

	guest_is_nested = is_nested;
	sync_global_to_guest(vm, guest_is_nested);
	vcpu_run(vcpu);

	/*
	 * The frame pushed to the BAD STACK without memslot cannot be
	 * emulated during event vectoring, so KVM exits to userspace here
	 * instead of completing delivery.
	 */
	TEST_ASSERT(vcpu->run->exit_reason == KVM_EXIT_INTERNAL_ERROR &&
		    	vcpu->run->internal.suberror == KVM_INTERNAL_ERROR_DELIVERY_EV,
		    	"Expected DELIVERY_EV exit, got reason %u suberror %u",
		    	vcpu->run->exit_reason,
		    	vcpu->run->exit_reason == KVM_EXIT_INTERNAL_ERROR ?
			    vcpu->run->internal.suberror : 0);

	vcpu_events_get(vcpu, &events);
	TEST_ASSERT(events.flags & KVM_VCPUEVENT_VALID_FRED_STATE,
		    	"FRED state not reported by KVM");
	TEST_ASSERT_EQ(events.exception.injected, 1);
	TEST_ASSERT_EQ(events.exception.nr, PF_VECTOR);
	TEST_ASSERT_EQ(events.exception_is_nested, is_nested);
	/* The event data of a #PF is its faulting linear address */
	TEST_ASSERT(events.exception_event_data != 0,
		    	"Injected #PF has zero event data; nothing to migrate");

	return events.exception_event_data;
}

static void check_migrated_state(struct kvm_vcpu *vcpu, u64 event_data,
				 bool is_nested)
{
	struct kvm_vcpu_events events;

	vcpu_events_get(vcpu, &events);

	TEST_ASSERT(events.flags & KVM_VCPUEVENT_VALID_FRED_STATE,
		    	"FRED state not reported by KVM after migration");
	TEST_ASSERT_EQ(events.exception.injected, 1);
	TEST_ASSERT_EQ(events.exception.nr, PF_VECTOR);
	TEST_ASSERT_EQ(events.exception_event_data, event_data);
	TEST_ASSERT_EQ(events.exception_is_nested, is_nested);
}

static void resume_and_check(struct kvm_vcpu *vcpu, u64 event_data,
			     bool is_nested)
{
	struct ucall uc;

	/*
 	 * Resume the destination VM and let KVM re-inject the #PF and
	 * complete the FRED delivery.
 	 */
	vcpu_run(vcpu);
	/* fred_entry_from_kernel() triggers VMEXIT. */
	TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

	switch (get_ucall(vcpu, &uc)) {
	case UCALL_ABORT:
		REPORT_GUEST_ASSERT(uc);
		break;
	case UCALL_SYNC:
		TEST_ASSERT(uc.args[0] == event_data,
			    	"Wrong FRED event data, expected 0x%lx, got 0x%lx",
			    	(unsigned long)event_data, (unsigned long)uc.args[0]);
		TEST_ASSERT(uc.args[1] == is_nested,
			    	"Wrong FRED nested flag, expected %d, got %ld",
			    	is_nested, uc.args[1]);
		break;
	default:
		TEST_FAIL("Unexpected ucall %lu", uc.cmd);
	}
}

static void map_bad_stack(struct kvm_vm *vm)
{
	/*
     * Map the stack page in the guest page table but do NOT back it
     * with a memslot.
     */
	virt_map(vm, BAD_STACK_GPA, BAD_STACK_GPA, BAD_STACK_NPAGES);
}

static void fix_bad_stack(struct kvm_vm *vm)
{
	/* Fix bad stack by installing the missing memslot. */
	vm_userspace_mem_region_add(vm, VM_MEM_SRC_ANONYMOUS, BAD_STACK_GPA,
				    			BAD_STACK_SLOT, BAD_STACK_NPAGES, 0);
}

static struct kvm_vm *create_fred_vm(struct kvm_vcpu **vcpu)
{
	struct kvm_vm *vm;

	vm = __vm_create_with_vcpus(VM_SHAPE(VM_MODE_PXXVYY_4K), 1, 0,
								guest_code, vcpu);
	vm_enable_cap(vm, KVM_CAP_X86_FRED_EVENT, 1);
	map_bad_stack(vm);

	return vm;
}

static void test_migration(bool is_nested)
{
	struct kvm_x86_state *state;
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	u64 event_data;

	vm = create_fred_vm(&vcpu);
	run_until_true(vcpu);

	event_data = inject_and_freeze(vm, vcpu, is_nested);
	state = vcpu_save_state(vcpu);

	kvm_vm_release(vm);

	vcpu = vm_recreate_with_one_vcpu(vm);
	vm_enable_cap(vm, KVM_CAP_X86_FRED_EVENT, 1);
	vcpu_load_state(vcpu, state);
	kvm_x86_state_cleanup(state);

	/*
	 * Before resuming the guest, fix the bad stack, to make sure the
	 * FRED frame can be properly pushed and delivered.
	 */
	fix_bad_stack(vm);

	check_migrated_state(vcpu, event_data, is_nested);
	resume_and_check(vcpu, event_data, is_nested);

	kvm_vm_free(vm);
}

int main(int argc, char *argv[])
{
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_FRED));
	TEST_REQUIRE(kvm_has_cap(KVM_CAP_X86_FRED_EVENT));

	test_migration(false);
	test_migration(true);

	return 0;
}
