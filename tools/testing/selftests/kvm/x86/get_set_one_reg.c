// SPDX-License-Identifier: GPL-2.0
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>

#include "test_util.h"
#include "kvm_util.h"
#include "processor.h"

int main(int argc, char *argv[])
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	u64 data;
	int r;

	TEST_REQUIRE(kvm_has_cap(KVM_CAP_ONE_REG));

	vm = vm_create_with_one_vcpu(&vcpu, NULL);

	TEST_ASSERT_EQ(__vcpu_get_reg(vcpu, KVM_X86_REG_MSR(MSR_EFER), &data), 0);
	TEST_ASSERT_EQ(__vcpu_set_reg(vcpu, KVM_X86_REG_MSR(MSR_EFER), data), 0);

	if (kvm_cpu_has(X86_FEATURE_SHSTK)) {
		r = __vcpu_get_reg(vcpu, KVM_X86_REG_SYNTHETIC_MSR(KVM_SYNTHETIC_GUEST_SSP),
				   &data);
		TEST_ASSERT_EQ(r, 0);
		r = __vcpu_set_reg(vcpu, KVM_X86_REG_SYNTHETIC_MSR(KVM_SYNTHETIC_GUEST_SSP),
				   data);
		TEST_ASSERT_EQ(r, 0);
	}

	kvm_vm_free(vm);
	return 0;
}
