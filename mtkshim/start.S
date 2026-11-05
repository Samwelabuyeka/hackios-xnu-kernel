/*
 * HakiOS MediaTek boot shim: entry.
 *
 * The binary is a Linux arm64 "Image" (Documentation/arm64/booting.rst): LK, the MediaTek
 * bootloader, loads it where it would load a Linux kernel and jumps to its first instruction with
 *   x0 = physical address of the flattened device tree, x1-x3 = 0,
 *   MMU off, caches off or clean, at EL1 (ARM Trusted Firmware keeps EL3; some LKs leave EL2).
 * QEMU's virt machine does the same for -kernel, which is how this is tested without a phone.
 *
 * Position independent: LK puts the image at the kernel address of its board file (0x40080000 on
 * MT67xx), QEMU at 0x40080000 too, but nothing here assumes either; the R_AARCH64_RELATIVE entries
 * the linker left are applied against wherever we find ourselves.
 */
	.section ".head", "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset: 512 KB into RAM, the traditional place */
	.quad	_image_size		/* image_size: text + data + bss + stack, so LK keeps it free */
	.quad	0xa			/* flags: little-endian (bit 0 = 0), 4K pages (bits 1-2 = 01), anywhere (bit 3) */
	.quad	0, 0, 0			/* reserved */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* reserved (PE header offset for an EFI stub; none here) */

	.section ".text.start", "ax"
	.globl _start
_start:
	mov	x19, x0			/* the device tree, kept across the setup below */
	mrs	x20, CurrentEL
	mrs	x21, mpidr_el1

	/* where are we?  _head's link address is 0, so its run address is the load offset */
	adrp	x22, _head
	add	x22, x22, :lo12:_head

	/* stack: the top of the image, 64 KB past bss (see linker.ld) */
	adrp	x1, _stack_top
	add	x1, x1, :lo12:_stack_top
	mov	sp, x1

	/* zero bss */
	adrp	x1, _bss_start
	add	x1, x1, :lo12:_bss_start
	adrp	x2, _bss_end
	add	x2, x2, :lo12:_bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:
	/* apply R_AARCH64_RELATIVE relocations: *(where + offset) = addend + offset */
	adrp	x1, _rela_start
	add	x1, x1, :lo12:_rela_start
	adrp	x2, _rela_end
	add	x2, x2, :lo12:_rela_end
3:	cmp	x1, x2
	b.hs	5f
	ldp	x3, x4, [x1], #16	/* r_offset, r_info */
	ldr	x5, [x1], #8		/* r_addend */
	cmp	x4, #1027		/* R_AARCH64_RELATIVE */
	b.ne	3b
	add	x5, x5, x22
	str	x5, [x3, x22]
	b	3b
5:
	/* disable alignment checking: the payload parser reads packed structures */
	mrs	x1, sctlr_el1
	bic	x1, x1, #(1 << 1)	/* SCTLR_EL1.A */
	msr	sctlr_el1, x1
	isb

	mov	x0, x19			/* fdt */
	lsr	x1, x20, #2		/* exception level */
	mov	x2, x21			/* mpidr */
	mov	x3, x22			/* load address */
	bl	shim_main
	/* shim_main does not return; if it does, park */
6:	wfe
	b	6b

	.globl smc_call
smc_call:				/* x0..x3 in, x0 out */
	smc	#0
	ret

	.globl hvc_call
hvc_call:
	hvc	#0
	ret

/*
 * A minimal exception vector table for the PAC probe (pacemu.h): a synchronous exception from the
 * current EL (SP_ELx) that is an undefined instruction (EC = 0) skips the instruction and notes it;
 * anything else prints nothing and parks, since there is no state worth continuing from.
 */
	.section ".text", "ax"
	.balign 2048
	.globl pac_vectors
pac_vectors:
	.rept 4				/* current EL with SP0: not used */
	b	pac_park
	.balign 128
	.endr
	b	pac_sync			/* current EL with SPx: synchronous */
	.balign 128
	b	pac_park			/* IRQ */
	.balign 128
	b	pac_park			/* FIQ */
	.balign 128
	b	pac_park			/* SError */
	.balign 128
	.rept 8				/* lower EL: not used */
	b	pac_park
	.balign 128
	.endr

pac_sync:
	stp	x0, x1, [sp, #-16]!
	mrs	x0, CurrentEL
	cmp	x0, #(2 << 2)
	b.eq	1f
	mrs	x0, esr_el1
	mrs	x1, elr_el1
	lsr	x0, x0, #26
	cbnz	x0, pac_park_pop	/* EC != 0: not an undefined instruction */
	add	x1, x1, #4
	msr	elr_el1, x1
	b	2f
1:	mrs	x0, esr_el2
	mrs	x1, elr_el2
	lsr	x0, x0, #26
	cbnz	x0, pac_park_pop
	add	x1, x1, #4
	msr	elr_el2, x1
2:	adrp	x0, pac_trap_seen
	add	x0, x0, :lo12:pac_trap_seen
	ldr	w1, [x0]
	add	w1, w1, #1
	str	w1, [x0]
	ldp	x0, x1, [sp], #16
	eret
pac_park_pop:
	ldp	x0, x1, [sp], #16
pac_park:
	wfe
	b	pac_park

	.globl pac_install_vectors
pac_install_vectors:			/* x0 = where to keep the old VBAR */
	mrs	x1, CurrentEL
	cmp	x1, #(2 << 2)
	adrp	x2, pac_vectors
	add	x2, x2, :lo12:pac_vectors
	b.eq	1f
	mrs	x1, vbar_el1
	str	x1, [x0]
	msr	vbar_el1, x2
	isb
	ret
1:	mrs	x1, vbar_el2
	str	x1, [x0]
	msr	vbar_el2, x2
	isb
	ret

	.globl pac_restore_vectors
pac_restore_vectors:			/* x0 = the old VBAR */
	mrs	x1, CurrentEL
	cmp	x1, #(2 << 2)
	b.eq	1f
	msr	vbar_el1, x0
	isb
	ret
1:	msr	vbar_el2, x0
	isb
	ret
