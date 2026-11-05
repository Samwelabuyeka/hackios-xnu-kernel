/* HakiOS MediaTek boot shim: linked at 0, relocated at run time (start.S) */
OUTPUT_ARCH(aarch64)
ENTRY(_head)
SECTIONS
{
	. = 0;
	.head : { KEEP(*(.head)) }
	.text : { *(.text.start) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	. = ALIGN(8);
	_rela_start = .;
	.rela.dyn : { *(.rela.dyn) *(.rela .rela.*) }
	_rela_end = .;
	. = ALIGN(16);
	_bss_start = .;
	.bss : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(16);
	_bss_end = .;
	. = ALIGN(4096);
	. += 0x10000;
	_stack_top = .;
	_image_size = .;
	/DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.interp) }
}
