/* The x32 guest image, embedded by both native hosts. */

	.section .rodata.halo_guest_image, "a"
	.balign 16
	.globl halo_guest_image
halo_guest_image:
	.incbin "halo_guest.elf"
	.globl halo_guest_image_end
halo_guest_image_end:

	.section .note.GNU-stack, "", @progbits
