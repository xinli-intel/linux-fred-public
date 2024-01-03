#if !defined(SHADOW_FIELD_RO) && !defined(SHADOW_FIELD_RW)
BUILD_BUG_ON(1)
#endif

#ifndef SHADOW_FIELD_RO
#define SHADOW_FIELD_RO(x, y, c)
#endif
#ifndef SHADOW_FIELD_RW
#define SHADOW_FIELD_RW(x, y, c)
#endif

/*
 * We do NOT shadow fields that are modified when L0
 * traps and emulates any vmx instruction (e.g. VMPTRLD,
 * VMXON...) executed by L1.
 * For example, VM_INSTRUCTION_ERROR is read
 * by L1 if a vmx instruction fails (part of the error path).
 * Note the code assumes this logic. If for some reason
 * we start shadowing these fields then we need to
 * force a shadow sync when L0 emulates vmx instructions
 * (e.g. force a sync if VM_INSTRUCTION_ERROR is modified
 * by nested_vmx_failValid)
 *
 * When adding or removing fields here, note that shadowed
 * fields must always be synced by prepare_vmcs02, not just
 * prepare_vmcs02_rare.
 */

/*
 * Keeping the fields ordered by size is an attempt at improving
 * branch prediction in vmcs12_read_any and vmcs12_write_any.
 */

/* 16-bits */
SHADOW_FIELD_RW(GUEST_INTR_STATUS, guest_intr_status, cpu_has_vmx_apicv())
SHADOW_FIELD_RW(GUEST_PML_INDEX, guest_pml_index, cpu_has_vmx_pml())
SHADOW_FIELD_RW(HOST_FS_SELECTOR, host_fs_selector, true)
SHADOW_FIELD_RW(HOST_GS_SELECTOR, host_gs_selector, true)

/* 32-bits */
SHADOW_FIELD_RO(VM_EXIT_REASON, vm_exit_reason, true)
SHADOW_FIELD_RO(VM_EXIT_INTR_INFO, vm_exit_intr_info, true)
SHADOW_FIELD_RO(VM_EXIT_INSTRUCTION_LEN, vm_exit_instruction_len, true)
SHADOW_FIELD_RO(VM_EXIT_INTR_ERROR_CODE, vm_exit_intr_error_code, true)
SHADOW_FIELD_RO(IDT_VECTORING_INFO_FIELD, idt_vectoring_info_field, true)
SHADOW_FIELD_RO(IDT_VECTORING_ERROR_CODE, idt_vectoring_error_code, true)
SHADOW_FIELD_RO(GUEST_CS_AR_BYTES, guest_cs_ar_bytes, true)
SHADOW_FIELD_RO(GUEST_SS_AR_BYTES, guest_ss_ar_bytes, true)
SHADOW_FIELD_RW(CPU_BASED_VM_EXEC_CONTROL, cpu_based_vm_exec_control, true)
SHADOW_FIELD_RW(PIN_BASED_VM_EXEC_CONTROL, pin_based_vm_exec_control, true)
SHADOW_FIELD_RW(EXCEPTION_BITMAP, exception_bitmap, true)
SHADOW_FIELD_RW(VM_ENTRY_EXCEPTION_ERROR_CODE, vm_entry_exception_error_code, true)
SHADOW_FIELD_RW(VM_ENTRY_INTR_INFO_FIELD, vm_entry_intr_info_field, true)
SHADOW_FIELD_RW(VM_ENTRY_INSTRUCTION_LEN, vm_entry_instruction_len, true)
SHADOW_FIELD_RW(TPR_THRESHOLD, tpr_threshold, true)
SHADOW_FIELD_RW(GUEST_INTERRUPTIBILITY_INFO, guest_interruptibility_info, true)
SHADOW_FIELD_RW(VMX_PREEMPTION_TIMER_VALUE, vmx_preemption_timer_value, cpu_has_vmx_preemption_timer())

/* Natural width */
SHADOW_FIELD_RO(EXIT_QUALIFICATION, exit_qualification, true)
SHADOW_FIELD_RO(GUEST_LINEAR_ADDRESS, guest_linear_address, true)
SHADOW_FIELD_RW(GUEST_RIP, guest_rip, true)
SHADOW_FIELD_RW(GUEST_RSP, guest_rsp, true)
SHADOW_FIELD_RW(GUEST_CR0, guest_cr0, true)
SHADOW_FIELD_RW(GUEST_CR3, guest_cr3, true)
SHADOW_FIELD_RW(GUEST_CR4, guest_cr4, true)
SHADOW_FIELD_RW(GUEST_RFLAGS, guest_rflags, true)
SHADOW_FIELD_RW(CR0_GUEST_HOST_MASK, cr0_guest_host_mask, true)
SHADOW_FIELD_RW(CR0_READ_SHADOW, cr0_read_shadow, true)
SHADOW_FIELD_RW(CR4_READ_SHADOW, cr4_read_shadow, true)
SHADOW_FIELD_RW(HOST_FS_BASE, host_fs_base, true)
SHADOW_FIELD_RW(HOST_GS_BASE, host_gs_base, true)

/* 64-bit */
SHADOW_FIELD_RO(GUEST_PHYSICAL_ADDRESS, guest_physical_address, true)
SHADOW_FIELD_RO(GUEST_PHYSICAL_ADDRESS_HIGH, guest_physical_address, true)

#undef SHADOW_FIELD_RO
#undef SHADOW_FIELD_RW
