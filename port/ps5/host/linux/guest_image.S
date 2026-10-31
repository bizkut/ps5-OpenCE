/* The x32 guest image (build/ps5/halo_guest.elf), linked into the Linux
host so that the executable runs on its own wherever it is copied, as the
desktop build's does (tools/net_compare.py, tools/stagekit copy only the
executable and the libraries beside it). The assembler finds the image
through -Wa,-I (tools/ps5_host_build.py). */

	.section .rodata.halo_guest_image, "a"
	.balign 16
	.globl halo_guest_image
halo_guest_image:
	.incbin "halo_guest.elf"
	.globl halo_guest_image_end
halo_guest_image_end:

	.section .note.GNU-stack, "", @progbits
