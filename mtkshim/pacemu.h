/*
 * HakiOS MediaTek shim: does this CPU have pointer authentication?
 *
 * Why it matters more than anything else here: iOS 17 and later ship only arm64e user space, and
 * iOS 26 (xnu-12377, hakios-arm64e-dada) depends on the CPU executing PAC instructions. ARMv8.3's
 * FEAT_PAuth is what Snapdragon 8 Elite / 8 Gen 1+ have and what dada's "standard-PAC key backend"
 * drives. The Redmi 14C's Helio G81 is Cortex-A75 + Cortex-A55: ARMv8.2, no FEAT_PAuth.
 *
 * On such a core the arm64e instruction set splits in two:
 *   - the HINT-space forms (paciasp, autiasp, pacibsp, autibsp, xpaclri, pacia1716, ...): execute as
 *     NOP. A function prologue/epilogue signed with them runs, just unsigned.
 *   - the register forms (pacia Xd,Xn; autia; pacda; autda; xpaci; blraa; braa; retaa; ldraa; eretaa):
 *     UNDEFINED, trap to EL1 as an exception with ESR_EL1.EC = 0x00.
 * dyld, libsystem, Objective-C runtime and Swift use the register forms for every function pointer
 * and vtable. So an arm64e user space on this core faults constantly without help, and the help can
 * only be one of:
 *   (A) the kernel's undefined-instruction handler decodes and emulates the PAC instruction
 *       (pac = identity or a cheap hash into the top bits; aut = strip; branch forms = branch), and
 *       returns. Correct, universal, and slow: every indirect call takes a trap (~1-2 us on an A75).
 *       The cost is what docs/mediatek.md estimates from the counts this probe prints;
 *   (B) rewrite the binaries on disk (a dyld-cache pass that turns register forms into their
 *       unauthenticated equivalents). Fast, but it is an Apple binary modified and redistributed,
 *       which LEGAL.md rules out of the tree; each user would run it on their own image;
 *   (C) run the arm64 user space (iOS 16.x, what HakiOS already boots on the POCO) and leave iOS 26
 *       to PAC-capable phones.
 * This file only measures. The decision is documented, not hidden in code.
 */
#ifndef HAKIOS_PACEMU_H
#define HAKIOS_PACEMU_H

struct pac_probe { uint32_t apa, api, gpa, gpi, apa3, hint_ok, reg_traps; };
static struct pac_probe PAC;

/* an undefined-instruction trap lands here while the probe runs; we skip the instruction */
volatile int pac_trap_seen;
extern char pac_vectors[];
void pac_install_vectors(uint64_t *old_vbar);
void pac_restore_vectors(uint64_t old_vbar);

static void pac_selftest(void)
{
	uint64_t isar1, isar2;
	__asm__ volatile("mrs %0, id_aa64isar1_el1" : "=r"(isar1));
	__asm__ volatile("mrs %0, s3_0_c0_c6_2" : "=r"(isar2));   /* ID_AA64ISAR2_EL1, by number for old assemblers */
	PAC.apa  = (isar1 >> 4) & 15;    /* QARMA5 address auth */
	PAC.api  = (isar1 >> 8) & 15;    /* implementation-defined address auth */
	PAC.gpa  = (isar1 >> 24) & 15;   /* QARMA5 generic auth */
	PAC.gpi  = (isar1 >> 28) & 15;
	PAC.apa3 = (isar2 >> 12) & 15;   /* QARMA3 */

	/* HINT-space: must not trap on any ARMv8 core (NOP without the feature) */
	uint64_t old; pac_install_vectors(&old);
	pac_trap_seen = 0;
	__asm__ volatile("paciasp\n\tautiasp" ::: "x30", "memory");
	PAC.hint_ok = !pac_trap_seen;
	/* register forms: trap without FEAT_PAuth. Encoded as .inst so an old assembler cannot refuse them. */
	pac_trap_seen = 0;
	__asm__ volatile(
		"mov x1, #0\n\t"
		".inst 0xdac10021\n\t"   /* pacia x1, x1 */
		".inst 0xdac11021\n\t"   /* autia x1, x1 */
		".inst 0xdac143e1\n\t"   /* xpaci x1 */
		".inst 0xdac10821\n\t"   /* pacda x1, x1 */
		::: "x1", "memory");
	PAC.reg_traps = pac_trap_seen;
	pac_restore_vectors(old);

	int hw = PAC.apa || PAC.api || PAC.apa3;
	print("pac: ID_AA64ISAR1 APA=%d API=%d GPA=%d GPI=%d ISAR2 APA3=%d -> FEAT_PAuth %s\n",
	      PAC.apa, PAC.api, PAC.gpa, PAC.gpi, PAC.apa3, hw ? "present" : "ABSENT");
	print("pac: hint forms %s, register forms trapped %d of 4\n", PAC.hint_ok ? "run" : "TRAP", PAC.reg_traps);
	if (!hw)
		print("pac: arm64e user space (iOS 17+, iOS 26) needs emulation of the register forms on this core: see docs/mediatek.md\n");
}

#endif
