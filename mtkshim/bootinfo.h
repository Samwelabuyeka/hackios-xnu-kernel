/*
 * HakiOS MediaTek boot shim: what the shim hands to the stage after it.
 *
 * The shim is stage 0: it runs where LK (the MediaTek bootloader) would have run Linux, with
 * nothing but the device tree to go by. It finds the console, the memory, the framebuffer and the
 * XNUP payload, writes what it found into this structure, and if the payload carries a "stage1"
 * entry, copies that to free memory and calls it with
 *
 *     x0 = physical address of this structure (magic first), x1 = 0, x2 = 0, x3 = 0
 *     MMU off, caches as the shim found them, at the exception level the shim was given, SP valid.
 *
 * Stage 1 is where the XNU loader goes once it no longer needs UEFI: on Qualcomm the loader is a
 * UEFI application (xnuloader/loader.c) and the firmware is Mu-Silicium's; MediaTek has no UEFI
 * and this structure is what replaces the UEFI tables for it. The 34 boot-service calls the loader
 * makes today are listed in docs/mediatek.md with what each becomes here.
 *
 * Everything is little-endian, physical, and fixed-width so a tool on the host can read a dump of
 * it (tools/bootinfo-dump.py).
 */
#ifndef HAKIOS_BOOTINFO_H
#define HAKIOS_BOOTINFO_H
#include <stdint.h>

#define BOOTINFO_MAGIC   0x4F464E4942484B48ULL   /* "HKHBINFO" */
#define BOOTINFO_VERSION 1
#define BOOTINFO_MAX_MEM 16
#define BOOTINFO_MAX_RSV 64

enum bootinfo_uart { UART_NONE = 0, UART_PL011 = 1, UART_8250 = 2 };

struct bootinfo_range { uint64_t base, size; };

struct bootinfo {
	uint64_t magic;
	uint32_t version, size;                     /* sizeof(struct bootinfo) of the writer */

	/* where the shim itself is, so stage 1 may reuse the memory once it has copied what it needs */
	uint64_t shim_base, shim_size;
	uint64_t fdt;                               /* the device tree LK passed, untouched */
	uint64_t fdt_size;
	uint32_t exception_level, cpu_count;
	uint64_t mpidr;                             /* of the core the shim ran on */
	uint64_t midr;
	uint64_t cntfrq;                            /* the generic timer's rate */

	/* the console the shim used; stage 1 may keep using it with the same registers */
	uint32_t uart_type, uart_reg_shift;
	uint64_t uart_base;

	/* RAM as the device tree describes it, and what in it the firmware says is not ours */
	uint32_t mem_count, rsv_count;
	struct bootinfo_range mem[BOOTINFO_MAX_MEM];
	struct bootinfo_range rsv[BOOTINFO_MAX_RSV];

	/* the display, if LK left one lit: simple-framebuffer, or MediaTek's atag,videolfb */
	uint64_t fb_base, fb_size;
	uint32_t fb_width, fb_height, fb_stride, fb_bpp;

	/* the XNUP payload (xnuloader/mkpayload.py) as LK loaded it: it was the boot image's ramdisk */
	uint64_t payload_base, payload_size;

	/* PSCI: 0 = none, 1 = smc, 2 = hvc */
	uint32_t psci_method, psci_version;

	char bootargs[1024];                        /* /chosen/bootargs as given */
	char model[128];                            /* the root node's model */
	char compatible[256];                       /* the root node's compatible list, NUL-joined */
};

#endif
