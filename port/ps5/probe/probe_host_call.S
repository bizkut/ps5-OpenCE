/* uint64_t probe_call_guest(uint64_t (*entry)(uint64_t), uint64_t argument,
                             void *stack_top)

Calls a guest entry on a stack below 4 GB: x32 code keeps stack addresses in
32-bit registers (docs/ps5_port_plan.md, "The guest/host boundary"), so the
host's own stack, above 4 GB, would be cut short. The host's stack pointer
waits in rbp, which the guest saves and restores whole. */

	.text
	.globl probe_call_guest
	.type probe_call_guest, @function
probe_call_guest:
	pushq %rbp
	movq %rsp, %rbp
	movq %rdx, %rsp
	andq $-16, %rsp
	movq %rdi, %rax
	movq %rsi, %rdi
	callq *%rax
	movq %rbp, %rsp
	popq %rbp
	retq
	.size probe_call_guest, . - probe_call_guest

	.section .note.GNU-stack, "", @progbits
