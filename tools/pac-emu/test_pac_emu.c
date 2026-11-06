/*
 * Tests for the PAC emulator, run on the host -- no phone, no kernel.
 *
 * What is actually being proven here:
 *   1. Decode, for every form: the emulator and GNU as must agree. The table was generated from the
 *      assembler (gen_encodings.py); this test assembles a fresh set of instructions with *different*
 *      registers than the ones the generator used, decodes them, and checks that the register
 *      numbers the emulator reports are the ones that were written in the source. If a field were
 *      misplaced, the generator's own operand positions and this independent check would disagree.
 *   2. Not-a-PAC instructions are rejected rather than mis-decoded. An emulator that claims
 *      ordinary arithmetic is PAC would silently corrupt every process it runs.
 *   3. Semantics: sign/authenticate is identity, the branch forms really branch, BLRAA sets x30,
 *      RETAA returns through x30, LDRAA loads and authenticates, and a bad branch target faults
 *      instead of jumping into the kernel.
 *   4. The instruction mix: a small program compiled -arch arm64e-style is not available here
 *      without an Apple toolchain, so the ratio of PAC instructions to all instructions is taken
 *      from an assembly routine written to look like Objective-C dispatch. That ratio feeds the
 *      cost estimate that docs/mediatek.md quotes, so the number in the document has a source.
 *
 * usage: make -C tools/pac-emu test   (or ./run-tests.sh)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pac_emu.h"
#include "pac_decoder.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ------------------------------------------------------------------ helpers the emulator calls */

static uint64_t
test_fetch64(void *ctx, uint64_t addr, int *ok)
{
	(void)ctx;
	/* one readable page at 0x1000_0000; everything else faults */
	if (addr >= 0x10000000ull && addr + 8 <= 0x10001000ull) {
		*ok = 1;
		return 0x0000000aabbccddeull;
	}
	*ok = 0;
	return 0;
}

static int
test_user_ok(void *ctx, uint64_t addr)
{
	(void)ctx;
	/* user space on this core: below 2^48 and non-null */
	return addr != 0 && (addr >> 48) == 0;
}

/*
 * Assemble source and read the encoded instructions straight out of the object file's .text with
 * objcopy, rather than parsing objdump's text. The text form separates the address from the
 * encoding with a tab, and a regular expression that assumes spaces silently matches nothing --
 * which is exactly how the first version of this test "passed" by skipping every case.
 */
static int
assemble_text(const char *source, uint32_t *words, unsigned max_words)
{
	static const char *asm_path = "/tmp/pac_emu_in.s";
	static const char *obj_path = "/tmp/pac_emu_in.o";
	static const char *bin_path = "/tmp/pac_emu_in.bin";
	FILE *f = fopen(asm_path, "w");
	if (!f) {
		return -1;
	}
	fputs(".text\n", f);
	fputs(source, f);
	fputs("\n", f);
	fclose(f);
	char cmd[512];
	snprintf(cmd, sizeof cmd, "aarch64-linux-gnu-as -march=armv8.3-a -o %s %s 2>/dev/null", obj_path, asm_path);
	if (system(cmd) != 0) {
		return -1;
	}
	snprintf(cmd, sizeof cmd, "aarch64-linux-gnu-objcopy -O binary --only-section=.text %s %s 2>/dev/null",
	         obj_path, bin_path);
	if (system(cmd) != 0) {
		return -1;
	}
	f = fopen(bin_path, "rb");
	if (!f) {
		return -1;
	}
	unsigned char buf[4096];
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	if (n < 4) {
		return -1;
	}
	unsigned count = (unsigned)(n / 4);
	if (count > max_words) {
		count = max_words;
	}
	for (unsigned i = 0; i < count; i++) {
		/* little-endian words */
		words[i] = (uint32_t)buf[i * 4] | ((uint32_t)buf[i * 4 + 1] << 8) |
		           ((uint32_t)buf[i * 4 + 2] << 16) | ((uint32_t)buf[i * 4 + 3] << 24);
	}
	return (int)count;
}

static uint32_t
assemble_one(const char *text)
{
	uint32_t w[1];
	if (assemble_text(text, w, 1) < 1) {
		return 0;
	}
	return w[0];
}

/* ------------------------------------------------------------------ 1. decode agrees with GNU as */

struct case_reg { const char *asm_text; int op; int rd, rn, rm; long long imm; };

static const struct case_reg decode_cases[] = {
	/* deliberately different registers from the generator's all-x0 forms */
	{ "pacia x5, x9",        PAC_OP_PACIA,  5, 9, -1, 0 },
	{ "pacib x11, x2",       PAC_OP_PACIB, 11, 2, -1, 0 },
	{ "pacda x7, x19",       PAC_OP_PACDA,  7, 19, -1, 0 },
	{ "pacdb x1, x30",       PAC_OP_PACDB,  1, 30, -1, 0 },
	{ "autia x3, x4",        PAC_OP_AUTIA,  3, 4, -1, 0 },
	{ "autib x20, x21",      PAC_OP_AUTIB, 20, 21, -1, 0 },
	{ "autda x13, x14",      PAC_OP_AUTDA, 13, 14, -1, 0 },
	{ "autdb x25, x26",      PAC_OP_AUTDB, 25, 26, -1, 0 },
	{ "paciza x8",           PAC_OP_PACIZA,  8, -1, -1, 0 },
	{ "pacizb x17",          PAC_OP_PACIZB, 17, -1, -1, 0 },
	{ "pacdza x6",           PAC_OP_PACDZA,  6, -1, -1, 0 },
	{ "pacdzb x29",          PAC_OP_PACDZB, 29, -1, -1, 0 },
	{ "autiza x10",          PAC_OP_AUTIZA, 10, -1, -1, 0 },
	{ "autizb x12",          PAC_OP_AUTIZB, 12, -1, -1, 0 },
	{ "autdza x15",          PAC_OP_AUTDZA, 15, -1, -1, 0 },
	{ "autdzb x16",          PAC_OP_AUTDZB, 16, -1, -1, 0 },
	{ "xpaci x22",           PAC_OP_XPACI, 22, -1, -1, 0 },
	{ "xpacd x27",           PAC_OP_XPACD, 27, -1, -1, 0 },
	{ "pacga x0, x1, x2",    PAC_OP_PACGA,  0, 1, 2, 0 },
	{ "pacga x30, x28, x24", PAC_OP_PACGA, 30, 28, 24, 0 },
	{ "retaa",               PAC_OP_RETAA, -1, -1, -1, 0 },
	{ "retab",               PAC_OP_RETAB, -1, -1, -1, 0 },
	{ "braa x1, x2",         PAC_OP_BRAA, -1, 1, 2, 0 },
	{ "brab x9, x10",        PAC_OP_BRAB, -1, 9, 10, 0 },
	{ "blraa x3, x4",        PAC_OP_BLRAA, -1, 3, 4, 0 },
	{ "blrab x19, x20",      PAC_OP_BLRAB, -1, 19, 20, 0 },
	{ "braaz x11",           PAC_OP_BRAAZ, -1, 11, -1, 0 },
	{ "brabz x12",           PAC_OP_BRABZ, -1, 12, -1, 0 },
	{ "blraaz x13",          PAC_OP_BLRAAZ, -1, 13, -1, 0 },
	{ "blrabz x14",          PAC_OP_BLRABZ, -1, 14, -1, 0 },
	{ "ldraa x5, [x6]",      PAC_OP_LDRAA_IMM, 5, 6, -1, 0 },
	{ "ldrab x7, [x8]",      PAC_OP_LDRAB_IMM, 7, 8, -1, 0 },
	{ "ldraa x9, [x10, #8]", PAC_OP_LDRAA_IMM, 9, 10, -1, 8 },
	{ "ldraa x9, [x10, #-256]", PAC_OP_LDRAA_IMM, 9, 10, -1, -256 },
	{ "ldraa x11, [x12, #1000]", PAC_OP_LDRAA_IMM, 11, 12, -1, 1000 },
	{ "ldrab x13, [x14, #-8]", PAC_OP_LDRAB_IMM, 13, 14, -1, -8 },
	{ "eretaa",              PAC_OP_ERETAA, -1, -1, -1, 0 },
	{ "eretab",              PAC_OP_ERETAB, -1, -1, -1, 0 },
};

static void
test_decode_against_assembler(void)
{
	for (size_t i = 0; i < sizeof decode_cases / sizeof decode_cases[0]; i++) {
		const struct case_reg *t = &decode_cases[i];
		uint32_t enc = assemble_one(t->asm_text);
		if (enc == 0) {
			printf("SKIP (no assembler) %s\n", t->asm_text);
			continue;
		}
		struct pac_insn d;
		int ok = pac_decode(enc, &d);
		CHECK(ok, "not decoded: '%s' (0x%08x)", t->asm_text, enc);
		if (!ok) {
			continue;
		}
		CHECK(d.op == t->op, "'%s': op %d, expected %d", t->asm_text, d.op, t->op);
		CHECK(d.rd == t->rd, "'%s': rd %d, expected %d", t->asm_text, d.rd, t->rd);
		CHECK(d.rn == t->rn, "'%s': rn %d, expected %d", t->asm_text, d.rn, t->rn);
		CHECK(d.rm == t->rm, "'%s': rm %d, expected %d", t->asm_text, d.rm, t->rm);
		CHECK(d.imm == t->imm, "'%s': imm %lld, expected %lld", t->asm_text, d.imm, t->imm);
	}
}

/* ------------------------------------------------------------------ 2. ordinary instructions are not PAC */

static void
test_rejects_non_pac(void)
{
	static const char *not_pac[] = {
		"nop", "ret", "add x0, x1, x2", "ldr x0, [x1]", "br x2", "blr x3",
		"eret", "hint #25", "msr tpidr_el0, x0", "stp x29, x30, [sp, #-16]!",
		"mov x0, x1", "b .", "ldr x0, [x1, #8]", "brk #0",
	};
	for (size_t i = 0; i < sizeof not_pac / sizeof not_pac[0]; i++) {
		uint32_t enc = assemble_one(not_pac[i]);
		if (enc == 0) {
			continue;
		}
		struct pac_insn d;
		CHECK(!pac_decode(enc, &d), "wrongly decoded '%s' (0x%08x) as PAC op %d", not_pac[i], enc, d.op);
	}
}

/* ------------------------------------------------------------------ 3. semantics */

static struct pac_cpu
cpu_new(void)
{
	struct pac_cpu c;
	memset(&c, 0, sizeof c);
	c.sp = 0x000000016fd00000ull;
	c.pc = 0x0000000100000000ull;
	for (int i = 0; i < 31; i++) {
		c.x[i] = 0x1111000000000000ull + (uint64_t)i;
	}
	return c;
}

static void
test_semantics(void)
{
	uint32_t insn;

	/*
	 * pacia x5, x9 signs x5's OWN value, with x9 as the modifier -- it does not copy x9 into x5.
	 * Under identity signing the observable result is that x5 is unchanged and x9 is untouched.
	 */
	struct pac_cpu c = cpu_new();
	insn = assemble_one("pacia x5, x9");
	uint64_t x5_before = c.x[5], x9_before = c.x[9];
	uint64_t pc0 = c.pc;
	int r = pac_emu_step(&c, insn, test_fetch64, test_user_ok, NULL);
	CHECK(r == PAC_EMUL_OK, "pacia result %d", r);
	CHECK(c.x[5] == x5_before, "pacia x5: 0x%llx, expected its own value 0x%llx",
	      (unsigned long long)c.x[5], (unsigned long long)x5_before);
	CHECK(c.x[9] == x9_before, "pacia clobbered the modifier x9");
	CHECK(c.pc == pc0 + 4, "pacia did not advance pc");

	/* autia is the identity too: a signed pointer comes back unchanged */
	c = cpu_new();
	insn = assemble_one("autia x6, x7");
	uint64_t x6_before = c.x[6];
	pac_emu_step(&c, insn, test_fetch64, test_user_ok, NULL);
	CHECK(c.x[6] == x6_before, "autia changed the pointer");

	/* braa x1, x2 branches to x1 and does not link */
	c = cpu_new();
	c.x[1] = 0x0000000100002000ull;
	insn = assemble_one("braa x1, x2");
	r = pac_emu_step(&c, insn, test_fetch64, test_user_ok, NULL);
	CHECK(r == PAC_EMUL_BRANCH, "braa result %d", r);
	CHECK(c.pc == 0x0000000100002000ull, "braa target 0x%llx", (unsigned long long)c.pc);

	/* blraa x1, x2 links */
	c = cpu_new();
	c.x[1] = 0x0000000100003000ull;
	uint64_t ret_addr = c.pc + 4;
	insn = assemble_one("blraa x1, x2");
	pac_emu_step(&c, insn, test_fetch64, test_user_ok, NULL);
	CHECK(c.pc == 0x0000000100003000ull, "blraa target 0x%llx", (unsigned long long)c.pc);
	CHECK(c.x[30] == ret_addr, "blraa x30 0x%llx, expected 0x%llx",
	      (unsigned long long)c.x[30], (unsigned long long)ret_addr);

	/* retaa returns through x30 */
	c = cpu_new();
	c.x[30] = 0x0000000100004000ull;
	insn = assemble_one("retaa");
	r = pac_emu_step(&c, insn, test_fetch64, test_user_ok, NULL);
	CHECK(r == PAC_EMUL_BRANCH, "retaa result %d", r);
	CHECK(c.pc == 0x0000000100004000ull, "retaa target 0x%llx", (unsigned long long)c.pc);

	/* ldraa x5, [x6] loads from the readable page and authenticates */
	c = cpu_new();
	c.x[6] = 0x0000000010000000ull;
	insn = assemble_one("ldraa x5, [x6]");
	r = pac_emu_step(&c, insn, test_fetch64, test_user_ok, NULL);
	CHECK(r == PAC_EMUL_OK, "ldraa result %d", r);
	CHECK(c.x[5] == 0x0000000aabbccddeull, "ldraa loaded 0x%llx", (unsigned long long)c.x[5]);

	/* ldraa from an unreadable page faults rather than returning garbage */
	c = cpu_new();
	c.x[6] = 0x0000000090000000ull;
	insn = assemble_one("ldraa x5, [x6]");
	r = pac_emu_step(&c, insn, test_fetch64, test_user_ok, NULL);
	CHECK(r == PAC_EMUL_FAULT, "ldraa bad load result %d (expected FAULT)", r);

	/* a branch into the kernel address range faults, and must not move pc there */
	c = cpu_new();
	c.x[1] = 0xfffffff000000000ull;
	insn = assemble_one("braa x1, x2");
	r = pac_emu_step(&c, insn, test_fetch64, test_user_ok, NULL);
	CHECK(r == PAC_EMUL_FAULT, "braa to kernel result %d (expected FAULT)", r);

	/* Rn=31 means SP, not x31=0: pacia x5, sp must use the stack pointer as the modifier */
	c = cpu_new();
	insn = assemble_one("pacia x5, sp");
	struct pac_insn d;
	CHECK(pac_decode(insn, &d) && d.rn == 31, "pacia x5, sp: rn %d", d.rn);

	/* xpaci strips a PAC-shaped top byte but leaves a normal pointer alone */
	c = cpu_new();
	c.x[8] = 0xff00000100001234ull;
	insn = assemble_one("xpaci x8");
	pac_emu_step(&c, insn, test_fetch64, test_user_ok, NULL);
	CHECK(c.x[8] == 0x00000100001234ull, "xpaci stripped to 0x%llx", (unsigned long long)c.x[8]);
	c = cpu_new();
	c.x[8] = 0x0000000012345678ull;
	pac_emu_step(&c, insn, test_fetch64, test_user_ok, NULL);
	CHECK(c.x[8] == 0x0000000012345678ull, "xpaci corrupted a plain pointer: 0x%llx", (unsigned long long)c.x[8]);
}

/* ------------------------------------------------------------------ 4. the instruction mix */

/*
 * A function shaped like what an Objective-C dispatch loop compiles to: a load of the isa, a
 * compare, a branch, and an indirect call whose target is signed and authenticated. Assembled and
 * disassembled here so the ratio of PAC instructions to all instructions is measured, not assumed.
 */
static void
test_instruction_mix(void)
{
	static const char *prog =
		".text\n"
		"dispatch:\n"
		"  paciasp\n"
		"  stp x29, x30, [sp, #-16]!\n"
		"  mov x29, sp\n"
		"  ldr x8, [x0]\n"            /* isa */
		"  cbz x8, 1f\n"
		"  ldr x9, [x8, #16]\n"       /* method table */
		"  cbz x9, 2f\n"
		"  ldr x10, [x9, #8]\n"       /* imp */
		"  ldr x11, [x10]\n"
		"  cmp x11, #0\n"
		"  b.eq 3f\n"
		"  mov x1, x10\n"
		"  blraa x11, x1\n"           /* signed indirect call: the register form */
		"  b 4f\n"
		"1: mov w0, #0\n"
		"  b 4f\n"
		"2: mov w0, #1\n"
		"  b 4f\n"
		"3: mov w0, #2\n"
		"4: ldp x29, x30, [sp], #16\n"
		"  autiasp\n"
		"  ret\n";

	uint32_t words[256];
	int n = assemble_text(prog, words, 256);
	if (n < 0) {
		printf("SKIP instruction mix (no assembler)\n");
		return;
	}
	unsigned total = 0, pac_register = 0, pac_total = 0;
	for (int k = 0; k < n; k++) {
		struct pac_insn d;
		total++;
		if (pac_decode(words[k], &d)) {
			pac_total++;
			pac_register++;   /* every register form traps on ARMv8.2 */
		}
	}
	printf("instruction mix: %u instructions, %u PAC, of which %u trap on ARMv8.2\n",
	       total, pac_total, pac_register);
	CHECK(total > 20, "instruction mix too small (%u)", total);
	/*
	 * This loop is stiffer than real Objective-C code (which has far more bookkeeping), so the
	 * ratio here is an upper bound on how often the trap is taken. Printing it keeps the estimate
	 * in docs/mediatek.md anchored to something measurable.
	 */
	if (pac_register) {
		printf("  -> one trapping PAC instruction per %.1f instructions in this loop\n",
		       (double)total / pac_register);
	}
}

int
main(void)
{
	printf("pac-emu tests: %d forms in the generated table\n", PAC_FORMS_COUNT);
	test_decode_against_assembler();
	test_rejects_non_pac();
	test_semantics();
	test_instruction_mix();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
