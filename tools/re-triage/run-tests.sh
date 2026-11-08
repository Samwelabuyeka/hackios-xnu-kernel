#!/bin/bash
# Check re_triage.py against a driver whose register map is known exactly.
#
# Why this exists: a disassembler wrapper that "runs" is not a tool, it is a decoration. This test
# compiles test/known_map.c -- a stand-in driver for MediaTek's 8250-compatible UART (the
# mt6577-uart the boot shim already looks for), a display block, and a PMIC block reached through a
# register-offset `write_reg()` helper -- and then requires the analyzer to recover exactly the
# register map that source declares. Every one of the offsets below is in known_map.c.
#
# The test earned its place immediately: it caught two capstone behaviours that had silently broken
# value tracking, and a whole addressing mode that was being missed:
#
#   1. `movz x1, #0x200c` comes back with mnemonic "mov", not "movz" -- that branch never fired.
#   2. `movk`'s `lsl #16` is operand[1].shift, not a third operand -- reading ops[2] gave 0, so
#      0x1100|0x200c became 0x1100, fell below MIN_BASE, and the entire UART block vanished.
#   3. `str w1, [x0, x2]` keeps the constant block base in the *index* register. That is what a
#      vendor `write_reg(off, val)` compiles to, so missing it would miss every PMIC and clock
#      controller in a BSP.
#
# usage: tools/re-triage/run-tests.sh
set +e     # some assertions require a command to report a difference; errexit would abort early
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
cd "$REPO"

CROSS=${CROSS:-aarch64-linux-gnu-}
command -v ${CROSS}gcc >/dev/null || { echo "run-tests.sh: no ${CROSS}gcc (apt install gcc-aarch64-linux-gnu)"; exit 1; }
python3 -c 'import capstone' 2>/dev/null || { echo "run-tests.sh: no capstone (pip install capstone)"; exit 1; }

OBJ=/tmp/re-triage-known.o
${CROSS}gcc -c -O1 -mgeneral-regs-only -ffreestanding -fno-pic \
	-o "$OBJ" "$HERE/test/known_map.c" || { echo "run-tests.sh: could not build known_map.c"; exit 1; }

fail=0
ok()   { printf '  ok   %s\n' "$1"; }
bad()  { printf '  FAIL %s\n' "$1"; fail=$((fail + 1)); }

# Pull the recovered map out as "base:off,off,..." lines, offsets de-duplicated and sorted.
recovered=$(python3 "$HERE/re_triage.py" "$OBJ" --json 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
for b in d["blocks"]:
    offs = sorted({a["off"] for a in b["accesses"]})
    print("%x:%s" % (b["base"], ",".join("%x" % o for o in offs)))
print("unresolved:%d" % d["unresolved"])
print("notaperipheral:%d" % d["not_a_peripheral"])
print("loadsstores:%d" % d["loads_stores"])
')

echo "recovered map:"
echo "$recovered" | sed 's/^/  /'
echo

# ---- the blocks present, and every offset in each, must be exactly what the source declares

# UART 0x11002000: THR/RBR/DLL 00, IER/DLM 04, FCR 08, LCR 0C, MCR 10, LSR 14, SCR 1C
echo "$recovered" | grep -qx '11002000:0,4,8,c,10,14,1c' \
	&& ok "UART block 0x11002000 recovered with all 7 register offsets" \
	|| { echo "  FAIL UART block: expected 0,4,8,c,10,14,1c"; fail=$((fail+1)); }

# display 0x14000000: CFG 0, SIZE 4, STRIDE 8, FMT c, STATUS 10, IRQ_EN 1c
echo "$recovered" | grep -qx '14000000:0,4,8,c,10,1c' \
	&& ok "display block 0x14000000 recovered with all 6 offsets" \
	|| { echo "  FAIL display block: expected 0,4,8,c,10,1c"; fail=$((fail+1)); }

# the PMIC block is only reachable through register-offset addressing
echo "$recovered" | grep -qx '1000d000:0' \
	&& ok "PMIC block 0x1000d000 found through [x0, x2] register-offset addressing" \
	|| { echo "  FAIL PMIC block 0x1000d000 not found (register-offset addressing)"; fail=$((fail+1)); }

# ---- and nothing invented: no block that is not in the source
nblocks=$(echo "$recovered" | grep -cE '^[0-9a-f]+:')
[ "$nblocks" = "3" ] && ok "exactly 3 register blocks, none invented" \
	|| { echo "  FAIL expected 3 blocks, got $nblocks"; fail=$((fail+1)); }

# ---- the accounting must add up, or the percentages lie
echo "$recovered" | grep -qx 'loadsstores:20' && ok "20 loads/stores counted" \
	|| { echo "  FAIL expected 20 loads/stores"; fail=$((fail+1)); }
echo "$recovered" | grep -qx 'unresolved:0' && ok "nothing left unresolved" \
	|| { echo "  FAIL expected 0 unresolved"; fail=$((fail+1)); }
echo "$recovered" | grep -qx 'notaperipheral:0' && ok "nothing wrongly discarded as a null pointer" \
	|| { echo "  FAIL expected 0 discarded"; fail=$((fail+1)); }

# ---- the other container formats must at least parse and find something on the real binaries
for probe in prebuilt/loader.efi prebuilt/mach.development.bcm2837; do
	[ -f "$probe" ] || continue
	kind=$(python3 "$HERE/re_triage.py" "$probe" --json 2>/dev/null |
		python3 -c 'import json,sys; print(json.load(sys.stdin)["kind"])' 2>/dev/null)
	[ -n "$kind" ] && [ "$kind" != "unknown" ] && ok "$(basename "$probe") parses as $kind" \
		|| { echo "  FAIL $probe did not parse"; fail=$((fail+1)); }
done

# a raw blob needs --base, and must be refused politely without one rather than crashing
if [ -f prebuilt/loader.efi ]; then
	${CROSS}objcopy -O binary --only-section=.text prebuilt/loader.efi /tmp/re-raw.bin 2>/dev/null
	if [ -s /tmp/re-raw.bin ]; then
		python3 "$HERE/re_triage.py" /tmp/re-raw.bin --base 0xa1001000 >/dev/null 2>&1
		[ $? -eq 0 ] && ok "a raw blob with --base is disassembled" \
			|| { echo "  FAIL raw blob with --base failed"; fail=$((fail+1)); }
	fi
fi

echo
if [ "$fail" -ne 0 ]; then
	echo "run-tests.sh: $fail failure(s)"
	exit 1
fi
echo "run-tests.sh: ok"
echo "  The recovered register map matches test/known_map.c exactly. That is the whole claim this"
echo "  tool makes: it recovers *which registers a driver touches*. It does not recover what they"
echo "  mean -- 0x14 being UART line status is knowledge from the source, not from the binary."
