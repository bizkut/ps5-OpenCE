/* x32 guest image: code and data occupy separate 64 KiB-aligned segments. */
ENTRY(__guest_header)
PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}
SECTIONS
{
	. = 0x7c000000;
	.guest_header : { KEEP(*(.guest_header)) } :text
	.text : { *(.text .text.*) } :text
	.rodata : { *(.rodata .rodata.*) } :text
	. = ALIGN(0x10000);
	.data : { *(.data .data.*) } :data
	.guest_init : {
		__guest_init_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array))
		KEEP(*(.guest_init))
		__guest_init_end = .;
	} :data
	.bss : { *(.bss .bss.*) *(COMMON) } :data
	. = ALIGN(0x1000);
	__guest_image_end = .;
	ASSERT(__guest_image_end <= 0x80000000, "guest image exceeds 64 MiB")
	/DISCARD/ : { *(.comment) *(.note.*) *(.guest_fini) *(.eh_frame) }
}
