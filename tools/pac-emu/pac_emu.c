/*
 * Pointer-authentication emulation for ARMv8.2 cores (MediaTek MT6769 / Redmi 14C).
 *
 * Problem. iOS 17 and later ship only arm64e user space, and arm64e signs and checks every function
 * pointer with FEAT_PAuth (ARMv8.3). The Redmi 14C's Helio G81 is Cortex-A75 + A55: ARMv8.2, no
 * FEAT_PAuth. On that core the arm64e instruction set splits in two:
 *
 *   HINT-space forms -- paciasp, autiasp, pacibsp, autibsp, xpaclri, pacia1716, pacib1716,
 *   autia1716, autib1716, paciaz, ... -- are architecturally NOPs on every core older than ARMv8.3.
 *   Executing them is harmless, but they provide no protection: everything runs unsigned.
 *
 *   Register forms -- pacia/autia/pacda/autda/xpaci/pacga/braa/blraa/retaa/ldraa/eretaa and their
 *   variants -- are UNDEFINED and trap to EL1 with ESR_EL1.EC = 0x00 (UNCATEGORIZED).
 *
 * dyld, libobjc and Swift use the register forms for every indirect call, message send and vtable
 * slot. Without help a process dies on its first EXC_BAD_INSTRUCTION. This file is the help: a
 * decoder and semantics the kernel's ESR_EC_UNCATEGORIZED path can call before it gives up.
 *
 * Design, and the reasons:
 *
 *  1. pac_* signs with the identity. A hash would need a secret kept per process and getting it
 *     wrong turns working code into a crash. Identity means the pointer passes through unchanged,
 *     exactly as on a NOP core. On a CPU that cannot compute PAC the security value is zero either
 *     way; what matters is determinism.
 *  2. aut_* is therefore the identity too -- there is no code to strip, and stripping bits that
 *     happen to look like a code would corrupt a legitimate pointer.
 *  3. xpaci/xpacd strip the top byte only when it has the PAC-shaped pattern AND the address is not
 *     a kernel address. With top-byte-ignore, which AArch64 uses for 48-bit VAs, the top byte is
 *     ignored by the MMU anyway, so doing nothing for a plain pointer is safe.
 *  4. Kernel-mode PAC is NOT emulated. Masking a kernel bug with identity PAC creates a security
 *     hole that nothing later can detect. Kernel code must be built arm64 (as the POCO build
 *     already is) or run on a PAC-capable core.
 *  5. The trap itself is the cost: roughly 1.5 us per instruction on an A75 at 2 GHz. Pointer-heavy
 *     code (Objective-C dispatch, dyld fixups, Swift) takes one of these every 40-100 instructions,
 *     so the slowdown is a factor, not a percentage. pac_cost_percent() in the tests turns the
 *     measured instruction mix into that number instead of a guess.
 *
 * The decoder table is generated from GNU as by gen_encodings.py, which infers each operand's bit
 * position by assembling variants that move one register at a time. It fails rather than emitting a
 * table if a field is not where the assembler says. Two cases it caught that a typed table gets
 * wrong: PACGA's third register is at 20:16 (not 14:10), and LDRAA/LDRAB's immediate has a scale
 * bit outside the imm9 field.
 */
#include <string.h>
#include "pac_emu.h"

#include "pac_decoder.h"

/* ------------------------------------------------------------------ stats */

static struct pac_emu_stats pac_st;

void
pac_stats_reset(void)
{
	memset(&pac_st, 0, sizeof pac_st);
}

const struct pac_emu_stats *
pac_stats(void)
{
	return &pac_st;
}

/* ------------------------------------------------------------------ register file */

/*
 * AArch64 register encoding: 31 means SP where a register is read as a base or written as a
 * stack pointer, and XZR (zero, discards writes) everywhere else. Getting this wrong is the classic
 * emulator bug -- a pacia x0, sp would silently become pacia x0, x31=0 and the modifier would be
 * wrong -- so it is explicit here.
 */
static uint64_t
rd(const struct pac_cpu *c, int r)
{
	return r == 31 ? c->sp : c->x[r];
}

static void
wr(struct pac_cpu *c, int r, uint64_t v)
{
	if (r != 31) {
		c->x[r] = v;
	}
}

/* ------------------------------------------------------------------ decode */

int
pac_decode(uint32_t insn, struct pac_insn *out)
{
	pac_st.decode_calls++;
	for (unsigned i = 0; i < PAC_FORMS_COUNT; i++) {
		const struct pac_form *f = &pac_forms[i];
		if ((insn & f->mask) != f->value) {
			continue;
		}
		out->op = f->op;
		out->rd = out->rn = out->rm = -1;
		out->imm = 0;
		/* Every field is read at the bit position the generator inferred from the assembler. */
		if (f->has_rd) {
			out->rd = (insn >> f->rd_pos) & 31;
		}
		if (f->has_rn) {
			out->rn = (insn >> f->rn_pos) & 31;
		}
		if (f->has_rm) {
			out->rm = (insn >> f->rm_pos) & 31;
		}
		if (f->imm_width) {
			/*
			 * The offset is a signed field scaled by 8. Its bits are wherever the assembler put
			 * them -- for LDRAA the sign is at bit 22 and imm9 at 20:12, so the field is not
			 * contiguous. Rebuild the raw value, sign-extend from imm_width, then scale.
			 */
			uint32_t raw = 0;
			for (int b = 0; b < f->imm_width; b++) {
				if (f->imm_pos[b] >= 0 && ((insn >> f->imm_pos[b]) & 1)) {
					raw |= 1u << b;
				}
			}
			if (raw & (1u << (f->imm_width - 1))) {
				raw |= ~((1u << f->imm_width) - 1);      /* sign-extend */
			}
			out->imm = (long long)(int32_t)raw * 8;
		}
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ semantics */

/*
 * Identity signing and authentication. See the file comment, points 1 and 2. Kept as functions so a
 * later port that *does* have a key can change them in one place.
 */
static uint64_t
pac_sign(uint64_t ptr, uint64_t modifier)
{
	(void)modifier;
	return ptr;
}

static uint64_t
pac_auth(uint64_t ptr, uint64_t modifier)
{
	(void)modifier;
	return ptr;
}

int
pac_emu_step(struct pac_cpu *c, uint32_t insn,
	     uint64_t (*fetch64)(void *ctx, uint64_t addr, int *ok),
	     int (*user_ok)(void *ctx, uint64_t addr), void *ctx)
{
	struct pac_insn i;

	if (!pac_decode(insn, &i)) {
		return PAC_EMUL_NOT_MINE;
	}
	pac_st.emulated++;
	if ((unsigned)i.op < 64) {
		pac_st.by_op[i.op]++;
	}

	/*
	 * PACIA Xd, Xn computes Xd = PAC(Xd, Xn): the pointer being signed is Xd's own value and Xn is
	 * only the modifier. Reading it the other way round (Xd = PAC(Xn, ...)) happens to be invisible
	 * under identity signing but is still wrong, and would break a hashing implementation.
	 * For the branch forms Rn is the target, so those use `target` below.
	 */
	uint64_t self = i.rd >= 0 ? rd(c, i.rd) : 0;   /* the value signed, for the pac/aut/xpac family */
	uint64_t mod  = i.rn >= 0 ? rd(c, i.rn) : 0;   /* the modifier */
	uint64_t target = i.rn >= 0 ? rd(c, i.rn) : 0; /* the branch target, for braa/blraa/... */
	uint64_t tmod = i.rm >= 0 ? rd(c, i.rm) : 0;
	int branch = 0;

	switch (i.op) {
	case PAC_OP_PACIA: case PAC_OP_PACIB: case PAC_OP_PACDA: case PAC_OP_PACDB:
		wr(c, i.rd, pac_sign(self, mod));
		break;
	case PAC_OP_PACIZA: case PAC_OP_PACIZB: case PAC_OP_PACDZA: case PAC_OP_PACDZB:
		wr(c, i.rd, pac_sign(self, 0));
		break;
	case PAC_OP_AUTIA: case PAC_OP_AUTIB: case PAC_OP_AUTDA: case PAC_OP_AUTDB:
		wr(c, i.rd, pac_auth(self, mod));
		break;
	case PAC_OP_AUTIZA: case PAC_OP_AUTIZB: case PAC_OP_AUTDZA: case PAC_OP_AUTDZB:
		wr(c, i.rd, pac_auth(self, 0));
		break;
	case PAC_OP_XPACI: case PAC_OP_XPACD:
		/*
		 * ARM's XPACI: if bit 55 of the operand is zero, clear bits 63:56. Bit 55 selects the
		 * address range -- zero is the ordinary user range whose top byte carries the PAC (and,
		 * with TBI, an ordinary tag). Testing the top byte instead, or requiring it to be 0xff,
		 * is the mistake this comment exists to prevent: bit 55 and the top byte are not the
		 * same thing, and a pointer whose top byte is 0xff has bit 55 set anyway.
		 */
		wr(c, i.rd, ((self >> 55) & 1) == 0 ? (self & 0x00ffffffffffffffull) : self);
		break;
	case PAC_OP_PACGA:
		/* the code goes in the top half; with identity signing it is zero there */
		wr(c, i.rd, 0);
		break;
	case PAC_OP_RETAA: case PAC_OP_RETAB:
		/*
		 * RETAA returns through x30 and authenticates it with SP as the modifier. In an emulated
		 * exception this is a branch in the interrupted context: hand the target back to the caller.
		 */
		c->pc = pac_auth(c->x[30], c->sp);
		branch = 1;
		break;
	case PAC_OP_BRAA: case PAC_OP_BRAB:
		c->pc = pac_auth(target, tmod);
		branch = 1;
		break;
	case PAC_OP_BLRAA: case PAC_OP_BLRAB:
		c->x[30] = c->pc + 4;
		c->pc = pac_auth(target, tmod);
		branch = 1;
		break;
	case PAC_OP_BRAAZ: case PAC_OP_BRABZ:
		c->pc = pac_auth(target, c->sp);
		branch = 1;
		break;
	case PAC_OP_BLRAAZ: case PAC_OP_BLRABZ:
		c->x[30] = c->pc + 4;
		c->pc = pac_auth(target, c->sp);
		branch = 1;
		break;
	case PAC_OP_LDRAA: case PAC_OP_LDRAB: case PAC_OP_LDRAA_IMM: case PAC_OP_LDRAB_IMM: {
		int ok = 0;
		uint64_t addr = target + (uint64_t)i.imm;
		uint64_t word = fetch64(ctx, addr, &ok);
		if (!ok) {
			pac_st.faults++;
			return PAC_EMUL_FAULT;
		}
		wr(c, i.rd, pac_auth(word, addr));
		break;
	}
	case PAC_OP_ERETAA: case PAC_OP_ERETAB:
		/* authenticate x30 with SP and return through it */
		c->pc = pac_auth(c->x[30], c->sp);
		c->x[30] = 0;
		branch = 1;
		break;
	default:
		return PAC_EMUL_NOT_MINE;
	}

	if (branch) {
		if (!user_ok(ctx, c->pc)) {
			pac_st.faults++;
			return PAC_EMUL_FAULT;
		}
		return PAC_EMUL_BRANCH;
	}
	c->pc += 4;
	return PAC_EMUL_OK;
}
