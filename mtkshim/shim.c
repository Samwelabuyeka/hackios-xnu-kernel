/*
 * HakiOS MediaTek boot shim, stage 0.
 *
 * What it is for: HakiOS boots stock iOS user space on Apple's open XNU kernel. On Qualcomm phones
 * the kernel is brought in by a UEFI application (xnuloader/loader.c) under Mu-Silicium's UEFI
 * port, which exists because Qualcomm's own boot chain is UEFI and the port mostly reuses it.
 * MediaTek phones have no UEFI: ARM Trusted Firmware starts LK (Little Kernel), LK starts Linux.
 * There is nothing to run an EFI application. So on MediaTek the loader's job is split: this shim
 * is what LK can start -- it looks exactly like a Linux kernel to LK -- and it gathers what the
 * loader got from UEFI into one structure (bootinfo.h) for the stage after it.
 *
 * What it does, in order:
 *   1. finds a serial console from the device tree (stdout-path, else the first 8250/PL011) and
 *      says hello, so the first sign of life on a phone is a line on the UART, not a black screen;
 *   2. reads /memory and /reserved-memory;
 *   3. finds the framebuffer LK left lit (simple-framebuffer in the tree, or the MediaTek-specific
 *      /chosen atag,videolfb) and draws on it, so a phone with no UART soldered still shows the
 *      shim ran;
 *   4. finds the XNUP payload: LK loaded it as the boot image's "ramdisk" and put its address into
 *      /chosen/linux,initrd-start;
 *   5. fills in struct bootinfo, and if the payload carries a "stage1" entry, copies it out and
 *      jumps to it; otherwise reports and halts (the smoke test ends here).
 *
 * What it does not do: touch any MediaTek register but the UART's. Everything it needs comes from
 * the device tree, which is also why it runs unchanged under QEMU's virt machine: that is the
 * regression test, with the phone test being the same binary in a boot image (devices/redmi-14c/
 * mkboot.sh) sent with "fastboot boot". Nothing is flashed, as on the POCO.
 */
#include <stdint.h>
#include <stddef.h>
#include "bootinfo.h"

#define PAYLOAD_MAGIC 0x50554E58u /* "XNUP", xnuloader/mkpayload.py */
struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

uint64_t smc_call(uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t hvc_call(uint64_t, uint64_t, uint64_t, uint64_t);

static struct bootinfo BI __attribute__((aligned(16)));

/* ------------------------------------------------------------------ small libc */

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static int sprefix(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static void *mcpy(void *d, const void *s, size_t n) { uint8_t *dd = d; const uint8_t *ss = s; while (n--) *dd++ = *ss++; return d; }
static void *mset(void *d, int c, size_t n) { uint8_t *dd = d; while (n--) *dd++ = (uint8_t)c; return d; }
static void scpy(char *d, const char *s, size_t cap) { size_t i = 0; for (; s[i] && i + 1 < cap; i++) d[i] = s[i]; d[i] = 0; }
static uint32_t be32(const void *p) { const uint8_t *b = p; return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static uint64_t be64(const void *p) { return (uint64_t)be32(p) << 32 | be32((const uint8_t *)p + 4); }

/* ------------------------------------------------------------------ console */

static volatile uint32_t *uart;
static int uart_type, uart_shift;

static void uart_put(char c)
{
	if (!uart) return;
	if (uart_type == UART_PL011) {
		while (uart[0x18 / 4] & (1 << 5)) ;        /* FR.TXFF */
		uart[0] = c;
	} else {                                           /* 8250: LSR.THRE then THR */
		while (!(uart[5 << uart_shift >> 2] & 0x20)) ;
		uart[0] = c;
	}
}

static void puts_(const char *s) { for (; *s; s++) { if (*s == '\n') uart_put('\r'); uart_put(*s); } }
static void puthex(uint64_t v, int digits)
{
	char b[17]; int i = 16; b[16] = 0;
	do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v && i > 0);
	while (16 - i < digits && i > 0) b[--i] = '0';
	puts_(b + i);
}
static void putdec(uint64_t v) { char b[21]; int i = 20; b[20] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); puts_(b + i); }

/* print("...%x...%d...%s...", ...) with x = hex, d = decimal, s = string, enough for a boot log */
static void print(const char *f, ...)
{
	__builtin_va_list ap; __builtin_va_start(ap, f);
	for (; *f; f++) {
		if (*f != '%') { if (*f == '\n') uart_put('\r'); uart_put(*f); continue; }
		switch (*++f) {
		case 'x': puthex(__builtin_va_arg(ap, uint64_t), 1); break;
		case 'X': puthex(__builtin_va_arg(ap, uint64_t), 16); break;
		case 'd': putdec(__builtin_va_arg(ap, uint64_t)); break;
		case 's': puts_(__builtin_va_arg(ap, const char *)); break;
		default: uart_put('%'); uart_put(*f);
		}
	}
	__builtin_va_end(ap);
}

/* the PAC probe prints with the console above */
#include "pacemu.h"

/* ------------------------------------------------------------------ flattened device tree */

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

static const uint8_t *fdt;
static const uint8_t *fdt_struct, *fdt_strings;
static uint32_t fdt_struct_size;

static int fdt_init(const void *blob)
{
	fdt = blob;
	if (!fdt || be32(fdt) != 0xd00dfeed) return 0;
	fdt_struct = fdt + be32(fdt + 8);
	fdt_strings = fdt + be32(fdt + 12);
	fdt_struct_size = be32(fdt + 36);
	return 1;
}

/*
 * A node is an offset into the structure block of its FDT_BEGIN_NODE token. Walking it is the only
 * primitive: callbacks per property while depth == 0, recursion into children by offset.
 */
static uint32_t rd(uint32_t off) { return be32(fdt_struct + off); }
static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }

/* the first child of node `off` whose name (before any '@') is `name`, or whose full name is; -1 if none */
static int fdt_subnode(int off, const char *name)
{
	int depth = 0;
	uint32_t p = off;
	if (rd(p) != FDT_BEGIN_NODE) return -1;
	p += 4; p += align4(slen((const char *)fdt_struct + p) + 1);
	while (p < fdt_struct_size) {
		uint32_t tok = rd(p);
		if (tok == FDT_BEGIN_NODE) {
			const char *n = (const char *)fdt_struct + p + 4;
			if (depth == 0) {
				size_t i = 0; while (n[i] && n[i] != '@') i++;
				if ((seq(n, name)) || (i == slen(name) && sprefix(n, name) && (n[i] == '@' || n[i] == 0)))
					return (int)p;
			}
			depth++;
			p += 4 + align4(slen(n) + 1);
		} else if (tok == FDT_END_NODE) {
			if (depth == 0) return -1;
			depth--; p += 4;
		} else if (tok == FDT_PROP) {
			p += 12 + align4(rd(p + 4));
		} else if (tok == FDT_NOP) {
			p += 4;
		} else return -1;
	}
	return -1;
}

/* the property `name` of node `off`: pointer to its data and its length, or NULL */
static const void *fdt_prop(int off, const char *name, uint32_t *len)
{
	uint32_t p = off;
	if (rd(p) != FDT_BEGIN_NODE) return NULL;
	p += 4; p += align4(slen((const char *)fdt_struct + p) + 1);
	while (p < fdt_struct_size) {
		uint32_t tok = rd(p);
		if (tok == FDT_PROP) {
			uint32_t l = rd(p + 4), ns = rd(p + 8);
			if (seq((const char *)fdt_strings + ns, name)) { if (len) *len = l; return fdt_struct + p + 12; }
			p += 12 + align4(l);
		} else if (tok == FDT_NOP) p += 4;
		else break;                                    /* BEGIN_NODE / END_NODE: no more properties here */
	}
	return NULL;
}

static const char *fdt_name(int off) { return (const char *)fdt_struct + off + 4; }

/* the next node after `off` in tree order, at any depth; -1 at the end */
static int fdt_next(int off, int *depth)
{
	uint32_t p = off;
	if (rd(p) == FDT_BEGIN_NODE) { p += 4 + align4(slen(fdt_name(off)) + 1); }
	while (p < fdt_struct_size) {
		uint32_t tok = rd(p);
		if (tok == FDT_BEGIN_NODE) { if (depth) (*depth)++; return (int)p; }
		if (tok == FDT_END_NODE) { if (depth) (*depth)--; p += 4; }
		else if (tok == FDT_PROP) p += 12 + align4(rd(p + 4));
		else if (tok == FDT_NOP) p += 4;
		else return -1;
	}
	return -1;
}

static int fdt_is_compatible(int off, const char *what)
{
	uint32_t len; const char *c = fdt_prop(off, "compatible", &len);
	if (!c) return 0;
	for (uint32_t i = 0; i < len; i += slen(c + i) + 1) if (seq(c + i, what)) return 1;
	return 0;
}

/* resolve a path like /soc/serial@11002000 (aliases not needed: stdout-path on these trees is a path) */
static int fdt_path(const char *path)
{
	int off = 0;
	if (*path != '/') {
		int al = fdt_subnode(0, "aliases");
		const char *v = al >= 0 ? fdt_prop(al, path, NULL) : NULL;
		if (!v) return -1;
		path = v;
	}
	while (*path == '/') path++;
	while (*path) {
		char part[64]; size_t i = 0;
		while (*path && *path != '/' && *path != ':' && i + 1 < sizeof part) part[i++] = *path++;
		part[i] = 0;
		off = fdt_subnode(off, part);
		if (off < 0) return -1;
		if (*path == ':') break;
		while (*path == '/') path++;
	}
	return off;
}

/* read a reg-like property with #address-cells/#size-cells of the parent (default 2/2 on arm64) */
static int fdt_reg(int off, int ac, int sc, int index, uint64_t *base, uint64_t *size)
{
	uint32_t len; const uint8_t *r = fdt_prop(off, "reg", &len);
	if (!r) return 0;
	uint32_t stride = 4 * (ac + sc);
	if ((uint32_t)(index + 1) * stride > len) return 0;
	r += index * stride;
	*base = ac == 2 ? be64(r) : be32(r);
	*size = sc == 2 ? be64(r + 4 * ac) : sc == 1 ? be32(r + 4 * ac) : 0;
	return 1;
}

static int cells(int off, const char *name, int dflt)
{
	uint32_t len; const void *p = fdt_prop(off, name, &len);
	return p && len == 4 ? (int)be32(p) : dflt;
}

/* ------------------------------------------------------------------ console discovery */

static void console_from_node(int off)
{
	uint64_t base, size;
	if (!fdt_reg(off, cells(0, "#address-cells", 2), cells(0, "#size-cells", 2), 0, &base, &size)) {
		/* nodes under /soc on MediaTek trees use the soc's cells; try the common 2/1 then 1/1 */
		if (!fdt_reg(off, 2, 1, 0, &base, &size) && !fdt_reg(off, 1, 1, 0, &base, &size)) return;
	}
	if (fdt_is_compatible(off, "arm,pl011")) uart_type = UART_PL011;
	else if (fdt_is_compatible(off, "mediatek,mt6577-uart") || fdt_is_compatible(off, "ns16550a")
	      || fdt_is_compatible(off, "snps,dw-apb-uart") || fdt_is_compatible(off, "mediatek,mt6779-uart")) {
		uart_type = UART_8250;
		uart_shift = cells(off, "reg-shift", 2);   /* MediaTek's 8250 has 32-bit registers 4 bytes apart */
	} else return;
	uart = (volatile uint32_t *)base;
	BI.uart_type = uart_type; BI.uart_reg_shift = uart_shift; BI.uart_base = base;
}

static void find_console(void)
{
	int chosen = fdt_subnode(0, "chosen");
	const char *so = chosen >= 0 ? fdt_prop(chosen, "stdout-path", NULL) : NULL;
	if (so) { int n = fdt_path(so); if (n >= 0) console_from_node(n); }
	if (uart) return;
	/* no stdout-path (MediaTek LK does not write one): the first UART-looking node in the tree */
	int depth = 0;
	for (int n = 0; n >= 0; n = fdt_next(n, &depth)) {
		const char *nm = fdt_name(n);
		if (sprefix(nm, "serial@") || sprefix(nm, "uart@") || sprefix(nm, "pl011@") || sprefix(nm, "uart0@")) {
			console_from_node(n);
			if (uart) return;
		}
	}
}

/* ------------------------------------------------------------------ memory */

static void find_memory(void)
{
	int ac = cells(0, "#address-cells", 2), sc = cells(0, "#size-cells", 2);
	int depth = 0;
	for (int n = 0; n >= 0; n = fdt_next(n, &depth)) {
		uint32_t len; const char *dt = fdt_prop(n, "device_type", &len);
		if (!dt || !seq(dt, "memory")) continue;
		for (int i = 0; BI.mem_count < BOOTINFO_MAX_MEM; i++) {
			uint64_t b, s;
			if (!fdt_reg(n, ac, sc, i, &b, &s)) break;
			if (s) { BI.mem[BI.mem_count].base = b; BI.mem[BI.mem_count].size = s; BI.mem_count++; }
		}
	}
	int rm = fdt_subnode(0, "reserved-memory");
	if (rm >= 0) {
		int rac = cells(rm, "#address-cells", ac), rsc = cells(rm, "#size-cells", sc);
		int d = 0;
		for (int n = fdt_next(rm, &d); n >= 0 && d > 0; n = fdt_next(n, &d)) {
			if (d != 1) continue;
			uint64_t b, s;
			if (fdt_reg(n, rac, rsc, 0, &b, &s) && BI.rsv_count < BOOTINFO_MAX_RSV) {
				BI.rsv[BI.rsv_count].base = b; BI.rsv[BI.rsv_count].size = s; BI.rsv_count++;
			}
		}
	}
	/* the memory reservation block at the head of the blob (rarely used, but LK may) */
	const uint8_t *mr = fdt + be32(fdt + 16);
	for (; be64(mr) || be64(mr + 8); mr += 16) {
		if (BI.rsv_count >= BOOTINFO_MAX_RSV) break;
		BI.rsv[BI.rsv_count].base = be64(mr); BI.rsv[BI.rsv_count].size = be64(mr + 8); BI.rsv_count++;
	}
}

/* ------------------------------------------------------------------ framebuffer */

static void find_framebuffer(void)
{
	int chosen = fdt_subnode(0, "chosen");
	/* 1. a simple-framebuffer node, under /chosen (the binding's home) or anywhere */
	int depth = 0;
	for (int n = 0; n >= 0; n = fdt_next(n, &depth)) {
		if (!fdt_is_compatible(n, "simple-framebuffer")) continue;
		uint64_t b, s;
		int parent_ac = 2, parent_sc = 2;
		if (!fdt_reg(n, parent_ac, parent_sc, 0, &b, &s) && !fdt_reg(n, 2, 1, 0, &b, &s) && !fdt_reg(n, 1, 1, 0, &b, &s)) continue;
		BI.fb_base = b; BI.fb_size = s;
		BI.fb_width = cells(n, "width", 0); BI.fb_height = cells(n, "height", 0);
		BI.fb_stride = cells(n, "stride", 0);
		const char *fmt = fdt_prop(n, "format", NULL);
		BI.fb_bpp = fmt && (seq(fmt, "r5g6b5")) ? 16 : 32;
		return;
	}
	/* 2. MediaTek LK: /chosen { atag,videolfb-fb_base_h, -fb_base_l, -islcmfound, -fps, -vramSize, -lcmname } */
	if (chosen >= 0) {
		uint32_t len; const void *p;
		uint64_t base = 0;
		if ((p = fdt_prop(chosen, "atag,videolfb-fb_base_l", &len)) && len == 4) base = be32(p);
		if ((p = fdt_prop(chosen, "atag,videolfb-fb_base_h", &len)) && len == 4) base |= (uint64_t)be32(p) << 32;
		if (!base && (p = fdt_prop(chosen, "atag,videolfb-fb_base", &len)) && len == 4) base = be32(p);
		if (base) {
			BI.fb_base = base;
			if ((p = fdt_prop(chosen, "atag,videolfb-vramSize", &len)) && len == 4) BI.fb_size = be32(p);
			/*
			 * LK's tree does not say the geometry; the panel driver in Linux does. The port
			 * puts it on the command line (fb=WxH) until the stage-1 loader reads the LCM
			 * table; the Redmi 14C's panel is 720x1640 (devices/redmi-14c/README.md).
			 */
			BI.fb_bpp = 32;
		}
	}
}

/* from /chosen/bootargs: fb=WxH[xSTRIDE] fills in what LK's tree leaves out */
static void fb_from_bootargs(void)
{
	const char *a = BI.bootargs;
	for (; *a; a++) {
		if ((a == BI.bootargs || a[-1] == ' ') && sprefix(a, "fb=")) {
			a += 3; uint64_t v[3] = {0, 0, 0}; int k = 0;
			for (; *a && *a != ' ' && k < 3; a++) {
				if (*a >= '0' && *a <= '9') v[k] = v[k] * 10 + (*a - '0');
				else if (*a == 'x') k++;
				else break;
			}
			if (v[0] && v[1]) { BI.fb_width = v[0]; BI.fb_height = v[1]; BI.fb_stride = v[2] ? v[2] : v[0] * 4; }
			return;
		}
	}
}

/*
 * Paint: a band of HakiOS colour across the top, and under it the boot info in a 5x7 font big
 * enough to read from a phone held at arm's length. A phone with no UART still says what it found.
 */
#include "font5x7.h"

static void px(uint32_t x, uint32_t y, uint32_t rgb)
{
	if (x >= BI.fb_width || y >= BI.fb_height) return;
	uint8_t *fb = (uint8_t *)BI.fb_base;
	if (BI.fb_bpp == 16) *(volatile uint16_t *)(fb + y * BI.fb_stride + x * 2) =
		((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x1f);
	else *(volatile uint32_t *)(fb + y * BI.fb_stride + x * 4) = rgb;
}

static void fb_text(uint32_t x0, uint32_t y0, uint32_t scale, uint32_t rgb, const char *s)
{
	for (uint32_t x = x0; *s; s++, x += 6 * scale) {
		if (*s < 32 || *s > 126) continue;
		const uint8_t *g = font5x7[*s - 32];
		for (int c = 0; c < 5; c++) for (int r = 0; r < 7; r++) if (g[c] & (1 << r))
			for (uint32_t sy = 0; sy < scale; sy++) for (uint32_t sx = 0; sx < scale; sx++)
				px(x + c * scale + sx, y0 + r * scale + sy, rgb);
	}
}

static void fb_hex(char *out, uint64_t v) { for (int i = 15; i >= 0; i--) out[15 - i] = "0123456789abcdef"[(v >> (i * 4)) & 15]; out[16] = 0; }

static void paint(void)
{
	if (!BI.fb_base || !BI.fb_width || !BI.fb_height) return;
	for (uint32_t y = 0; y < BI.fb_height; y++) for (uint32_t x = 0; x < BI.fb_width; x++)
		px(x, y, y < BI.fb_height / 8 ? 0x00c853 : 0x000000);
	uint32_t s = BI.fb_width >= 1080 ? 4 : BI.fb_width >= 720 ? 3 : 2, line = 9 * s, y = BI.fb_height / 8 + line;
	char buf[40];
	fb_text(10, y, s, 0xffffff, "HakiOS MediaTek shim"); y += line;
	fb_text(10, y, s, 0xaaaaaa, BI.model); y += line * 2;
	fb_text(10, y, s, 0xffffff, "memory:"); y += line;
	for (uint32_t i = 0; i < BI.mem_count && i < 4; i++) {
		fb_hex(buf, BI.mem[i].base); buf[16] = ' '; buf[17] = '+'; fb_hex(buf + 18, BI.mem[i].size);
		fb_text(10, y, s, 0xaaaaaa, buf); y += line;
	}
	y += line;
	fb_text(10, y, s, 0xffffff, BI.payload_base ? "payload: found" : "payload: NOT FOUND"); y += line;
	if (BI.payload_base) { fb_hex(buf, BI.payload_base); fb_text(10, y, s, 0xaaaaaa, buf); y += line; }
	y += line;
	fb_text(10, y, s, 0xffffff, BI.uart_base ? "uart: found" : "uart: none"); y += line;
	fb_text(10, y, s, 0xaaaaaa, "EL"); buf[0] = '0' + BI.exception_level; buf[1] = 0; fb_text(10 + 12 * s, y, s, 0xaaaaaa, buf);
}

/* ------------------------------------------------------------------ payload */

static const void *payload_entry(const char *name, uint32_t *size)
{
	const struct payload_header *ph = (const struct payload_header *)BI.payload_base;
	if (!ph || ph->magic != PAYLOAD_MAGIC) return NULL;
	const uint8_t *p = (const uint8_t *)(ph + 1), *end = p + ph->size;
	while (p + sizeof(struct payload_entry) <= end) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		if (!e->name[0]) break;
		if (seq(e->name, name)) { if (size) *size = e->size; return p + sizeof *e; }
		p += sizeof *e + ((e->size + 15) & ~15u);
	}
	return NULL;
}

static void find_payload(void)
{
	int chosen = fdt_subnode(0, "chosen");
	if (chosen < 0) return;
	uint32_t len; const void *p;
	uint64_t start = 0, end = 0;
	if ((p = fdt_prop(chosen, "linux,initrd-start", &len))) start = len == 8 ? be64(p) : be32(p);
	if ((p = fdt_prop(chosen, "linux,initrd-end", &len))) end = len == 8 ? be64(p) : be32(p);
	if (start && end > start && *(volatile uint32_t *)start == PAYLOAD_MAGIC) {
		BI.payload_base = start; BI.payload_size = end - start;
		return;
	}
	/*
	 * Not there, or LK handed the ramdisk address in its own way: walk RAM for the magic at 2 MB
	 * boundaries, the alignment LK and QEMU both use. Only the first 1 GB of each bank.
	 */
	for (uint32_t i = 0; i < BI.mem_count; i++) {
		uint64_t lim = BI.mem[i].size < 0x40000000 ? BI.mem[i].size : 0x40000000;
		for (uint64_t off = 0; off < lim; off += 0x200000) {
			uint64_t a = BI.mem[i].base + off;
			if (a >= BI.shim_base && a < BI.shim_base + BI.shim_size) continue;
			const struct payload_header *ph = (const struct payload_header *)a;
			if (ph->magic == PAYLOAD_MAGIC && ph->size < 0x40000000) {
				BI.payload_base = a; BI.payload_size = ph->size + sizeof *ph;
				return;
			}
		}
	}
}

/* ------------------------------------------------------------------ PSCI */

static void find_psci(void)
{
	int n = fdt_subnode(0, "psci");
	if (n < 0) return;
	const char *m = fdt_prop(n, "method", NULL);
	BI.psci_method = m && seq(m, "hvc") ? 2 : 1;
	uint64_t v = BI.psci_method == 2 ? hvc_call(0x84000000, 0, 0, 0) : smc_call(0x84000000, 0, 0, 0);
	BI.psci_version = (uint32_t)v;
}

static void count_cpus(void)
{
	int cpus = fdt_subnode(0, "cpus");
	if (cpus < 0) return;
	int d = 0;
	for (int n = fdt_next(cpus, &d); n >= 0 && d > 0; n = fdt_next(n, &d)) {
		if (d != 1) continue;
		const char *dt = fdt_prop(n, "device_type", NULL);
		if (dt && seq(dt, "cpu")) BI.cpu_count++;
	}
}

/* ------------------------------------------------------------------ main */

extern char _head[], _image_size[];

void shim_main(uint64_t fdt_phys, uint64_t el, uint64_t mpidr, uint64_t load)
{
	mset(&BI, 0, sizeof BI);
	BI.magic = BOOTINFO_MAGIC; BI.version = BOOTINFO_VERSION; BI.size = sizeof BI;
	BI.shim_base = load; BI.shim_size = (uint64_t)_image_size - (uint64_t)_head;
	BI.fdt = fdt_phys; BI.exception_level = el; BI.mpidr = mpidr;
	__asm__ volatile("mrs %0, midr_el1" : "=r"(BI.midr));
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(BI.cntfrq));

	if (!fdt_init((const void *)fdt_phys)) {
		/* no tree, no console to say so on: the only sign is the framebuffer, and we cannot find it */
		for (;;) __asm__ volatile("wfe");
	}
	BI.fdt_size = be32(fdt + 4);
	find_console();
	print("\nHakiOS MediaTek shim (stage 0) at %x, %d bytes, EL%d, mpidr %x\n", load, BI.shim_size, el, mpidr);
	print("device tree at %x (%d bytes)\n", fdt_phys, BI.fdt_size);

	{
		uint32_t len; const char *m = fdt_prop(0, "model", &len);
		if (m) scpy(BI.model, m, sizeof BI.model);
		const char *c = fdt_prop(0, "compatible", &len);
		if (c) { uint32_t n = len < sizeof BI.compatible - 1 ? len : sizeof BI.compatible - 1; mcpy(BI.compatible, c, n); }
		int chosen = fdt_subnode(0, "chosen");
		const char *ba = chosen >= 0 ? fdt_prop(chosen, "bootargs", NULL) : NULL;
		if (ba) scpy(BI.bootargs, ba, sizeof BI.bootargs);
		print("model: %s\n", BI.model[0] ? BI.model : "(none)");
		print("compatible: %s\n", BI.compatible);
		print("bootargs: %s\n", BI.bootargs);
	}
	print("uart: %s at %x (shift %d)\n", uart_type == UART_PL011 ? "pl011" : uart_type == UART_8250 ? "8250" : "none", BI.uart_base, BI.uart_reg_shift);

	find_memory();
	for (uint32_t i = 0; i < BI.mem_count; i++) print("memory: %X + %x\n", BI.mem[i].base, BI.mem[i].size);
	for (uint32_t i = 0; i < BI.rsv_count; i++) print("reserved: %X + %x\n", BI.rsv[i].base, BI.rsv[i].size);

	count_cpus(); find_psci();
	print("cpus: %d, psci: %s v%d.%d, timer %d Hz, midr %x\n", BI.cpu_count,
	      BI.psci_method == 2 ? "hvc" : BI.psci_method == 1 ? "smc" : "none",
	      BI.psci_version >> 16, BI.psci_version & 0xffff, BI.cntfrq, BI.midr);

	pac_selftest();

	find_payload();
	if (BI.payload_base) {
		print("payload: XNUP at %x, %d bytes\n", BI.payload_base, BI.payload_size);
		const struct payload_header *ph = (const struct payload_header *)BI.payload_base;
		const uint8_t *p = (const uint8_t *)(ph + 1), *end = p + ph->size;
		while (p + sizeof(struct payload_entry) <= end) {
			const struct payload_entry *e = (const struct payload_entry *)p;
			if (!e->name[0]) break;
			print("  '%s' %d bytes\n", e->name, e->size);
			p += sizeof *e + ((e->size + 15) & ~15u);
		}
	} else print("payload: none (no linux,initrd-start, no XNUP magic in RAM)\n");

	find_framebuffer(); fb_from_bootargs();
	if (BI.fb_base) print("framebuffer: %x, %dx%d stride %d bpp %d\n", BI.fb_base, BI.fb_width, BI.fb_height, BI.fb_stride, BI.fb_bpp);
	else print("framebuffer: none in the tree\n");
	paint();

	/* stage 1: copy it to the first free 2 MB-aligned spot past the payload and go */
	uint32_t s1size; const void *s1 = payload_entry("stage1", &s1size);
	if (s1) {
		uint64_t dst = (BI.payload_base + BI.payload_size + 0x1fffff) & ~0x1fffffULL;
		print("stage1: %d bytes -> %x, bootinfo at %x\n", s1size, dst, (uint64_t)&BI);
		mcpy((void *)dst, s1, s1size);
		__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
		void (*go)(uint64_t, uint64_t, uint64_t, uint64_t) = (void *)dst;
		go((uint64_t)&BI, 0, 0, 0);
		print("stage1 returned\n");
	} else {
		print("no stage1 in the payload: stage 0 complete, halting. bootinfo at %x\n", (uint64_t)&BI);
	}
	print("SHIM-END\n");
	for (;;) __asm__ volatile("wfe");
}
