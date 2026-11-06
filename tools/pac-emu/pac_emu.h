/*
 * pac_emu.h: pointer-authentication emulation for ARMv8.2 cores (MediaTek MT6769 / Redmi 14C).
 * See pac_emu.c for why this exists and what it deliberately does not do.
 */
#ifndef HAKIOS_PAC_EMU_H
#define HAKIOS_PAC_EMU_H

#include <stdint.h>

enum {
	PAC_OP_NONE = 0,
	PAC_OP_PACIA, PAC_OP_PACIB, PAC_OP_PACDA, PAC_OP_PACDB,
	PAC_OP_AUTIA, PAC_OP_AUTIB, PAC_OP_AUTDA, PAC_OP_AUTDB,
	PAC_OP_PACIZA, PAC_OP_PACIZB, PAC_OP_PACDZA, PAC_OP_PACDZB,
	PAC_OP_AUTIZA, PAC_OP_AUTIZB, PAC_OP_AUTDZA, PAC_OP_AUTDZB,
	PAC_OP_XPACI, PAC_OP_XPACD,
	PAC_OP_PACGA,
	PAC_OP_RETAA, PAC_OP_RETAB,
	PAC_OP_BRAA, PAC_OP_BRAB, PAC_OP_BLRAA, PAC_OP_BLRAB,
	PAC_OP_BRAAZ, PAC_OP_BRABZ, PAC_OP_BLRAAZ, PAC_OP_BLRABZ,
	PAC_OP_LDRAA, PAC_OP_LDRAB, PAC_OP_LDRAA_IMM, PAC_OP_LDRAB_IMM,
	PAC_OP_ERETAA, PAC_OP_ERETAB,
};

struct pac_insn {
	int op;                 /* PAC_OP_* */
	int rd, rn, rm;         /* -1 when the instruction has no such operand */
	long long imm;          /* LDRAA/LDRAB: signed byte offset */
};

/* the register file of the interrupted context */
struct pac_cpu {
	uint64_t x[31];         /* x0..x30 */
	uint64_t sp;
	uint64_t pc;
};

enum pac_result {
	PAC_EMUL_OK = 0,        /* handled: continue at cpu->pc (already advanced by 4) */
	PAC_EMUL_BRANCH,        /* handled: continue at cpu->pc (a branch target) */
	PAC_EMUL_NOT_MINE,      /* not a PAC instruction */
	PAC_EMUL_FAULT,         /* it is PAC, but this execution faults (bad load / bad branch) */
};

int pac_decode(uint32_t insn, struct pac_insn *out);

int pac_emu_step(struct pac_cpu *cpu, uint32_t insn,
                 uint64_t (*fetch64)(void *ctx, uint64_t addr, int *ok),
                 int (*user_ok)(void *ctx, uint64_t addr), void *ctx);

struct pac_emu_stats {
	unsigned long decode_calls, emulated, faults;
	unsigned long by_op[64];
};
void pac_stats_reset(void);
const struct pac_emu_stats *pac_stats(void);

#endif
