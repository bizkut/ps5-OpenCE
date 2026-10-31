/* The probe guest image at PROBE_IMAGE_BASE (probe_abi.h): its header first,
then code and constants (read and execute), then data (read and write) on a
64 KiB boundary, the unit of the console's direct memory. */

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}

/* the host enters through the header's table, not the ELF entry */
ENTRY(probe_guest_header)

SECTIONS
{
	. = 0x7c000000;
	.probe_header : { KEEP(*(.probe_header)) } :text
	.text : { *(.text .text.*) } :text
	.rodata : { *(.rodata .rodata.*) } :text
	. = ALIGN(0x10000);
	.data : { *(.data .data.*) } :data
	.bss : { *(.bss .bss.* COMMON) } :data
	/DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) }
}
