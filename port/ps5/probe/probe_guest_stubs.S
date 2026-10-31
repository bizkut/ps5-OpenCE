/* The probe guest's imports (probe_abi.h): an x32 caller passes integers,
pointers and floats in the registers an x86-64 host function reads them
from, so a stub only jumps through the 8-byte slot the host filled with the
function's 64-bit address. A host function that takes a real pointer gets
the register zero-extended first: the x32 ABI does not promise the upper
half (entry_upper_bits in probe_guest.c measures what clang leaves there). */

	.text

	.macro import name, slot
	.globl \name
	.type \name, @function
\name:
	jmpq *probe_imports + 8 * \slot(%rip)
	.size \name, . - \name
	.endm

	import host_sum, 0
	import host_ticks, 1
	import host_nop, 2
	import host_report, 3
	import host_upper_bits, 4

	.globl host_dereference
	.type host_dereference, @function
host_dereference:
	movl %edi, %edi
	jmpq *probe_imports + 8 * 5(%rip)
	.size host_dereference, . - host_dereference

	.data
	.p2align 3
	.globl probe_imports
	.type probe_imports, @object
probe_imports:
	.zero 8 * 6
	.size probe_imports, . - probe_imports

	.section .note.GNU-stack, "", @progbits
