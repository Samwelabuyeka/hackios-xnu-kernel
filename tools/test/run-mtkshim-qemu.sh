#!/bin/bash
# Run the MediaTek boot shim under QEMU's virt machine, using the Linux arm64 boot protocol.
#
# Why this is a real test and not a formality: it starts the same shim.bin the phone gets, through
# the same entry convention LK uses (x0 = device tree, MMU off, EL1), and checks that it found the
# console, the RAM, the framebuffer and the XNUP payload. What it cannot check is any MediaTek
# register -- QEMU has no MT6769 -- which is exactly why the shim reads everything from the device
# tree: the code path under test is the same one the phone runs.
#
# The device tree: QEMU builds virt's own tree and hands it to the kernel in x0, and we let it --
# passing -dtb with a hand-written tree would *replace* virt's, and the shim would then find no
# console (virt's PL011 is /pl011@9000000, its PSCI is /psci, neither is in the synthetic MT6769
# tree). Losing virt's tree costs the whole run. So instead the tree is dumped from QEMU itself and
# then *extended* into a superset: virt's real PL011/GIC/PSCI/memory/initrd, plus the one thing LK
# adds that virt does not -- MediaTek's /chosen atag,videolfb-* framebuffer handoff. That way the
# framebuffer fallback path, which is the path the phone actually takes, is the one exercised here.
#
# Two CPUs are exercised, because they answer the question this project turns on:
#   cortex-a76   ARMv8.2, no FEAT_PAuth  -- the Redmi 14C's class (A75/A55)
#   cortex-a710  ARMv9, FEAT_PAuth      -- the Snapdragon 8 Elite's class, which dada targets
# The PAC lines in the output must differ between them; if they do not, the probe is broken.
#
# usage: tools/test/run-mtkshim-qemu.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
cd "$REPO"

command -v qemu-system-aarch64 >/dev/null || { echo "run-mtkshim-qemu.sh: no qemu-system-aarch64"; exit 1; }
for t in dtc fdtput; do
	command -v "$t" >/dev/null || { echo "run-mtkshim-qemu.sh: no $t (device tree compiler, devicetree-tools)"; exit 1; }
done
[ -f mtkshim/shim.bin ] || mtkshim/build.sh >/dev/null

PAYLOAD=${PAYLOAD:-mtkshim/test-payload.bin}
if [ ! -f "$PAYLOAD" ]; then
	echo "run-mtkshim-qemu.sh: building a small payload (the shim only lists its entries)"
	python3 xnuloader/mkpayload.py -o "$PAYLOAD" ramdisk=prebuilt/userland/hello \
		--cmdline "cpus=1 console=ttyAMA0" >/dev/null
fi

MEM=512M
CMDLINE="console=ttyAMA0 fb=720x1640"
DTB=/tmp/mtkshim-superset.dtb

# Everything that shapes the machine, in one place, so the dump and the run cannot disagree (the
# initrd address QEMU bakes into /chosen depends on the payload size, so a stale dump would point
# the shim at nothing). An array, not an echoed string: word splitting would tear the quoted
# -append value in half. -M is deliberately absent: it differs between the dump (extra dumpdtb=)
# and the run, and QEMU silently keeps only the last -M when given two.
MACHINE=(-m "$MEM" -kernel mtkshim/shim.bin -initrd "$PAYLOAD" -append "$CMDLINE")

# 1. QEMU's own tree, bootargs and initrd addresses included, dumped before the machine runs.
rm -f "$DTB"
qemu-system-aarch64 "${MACHINE[@]}" -M virt,gic-version=3,dumpdtb="$DTB" -cpu cortex-a76 \
	-display none -serial none -monitor none </dev/null >/dev/null 2>&1 || true
[ -s "$DTB" ] || { echo "run-mtkshim-qemu.sh: QEMU did not dump a device tree"; exit 1; }

# 2. Add what LK gives and virt does not: the framebuffer LK left lit, in MediaTek's ATAG shape.
#    No width/height/stride here on purpose -- LK's tree has none either, so the geometry must come
#    from the fb=WxH command line, which is the second half of the fallback path.
fdtput -t x "$DTB" /chosen atag,videolfb-fb_base_l 0x50000000
fdtput -t x "$DTB" /chosen atag,videolfb-fb_base_h 0x0
fdtput -t x "$DTB" /chosen atag,videolfb-vramSize   0x500000
fdtput -t x "$DTB" /chosen atag,videolfb-islcmfound 0x1
fdtput -t s "$DTB" / model "Xiaomi Redmi 14C (QEMU virt superset, LK atag shape)"

# nothing may have been lost from virt's tree by the edits
for probe in /pl011@9000000 /psci "/memory@40000000" /chosen; do
	fdtget -p "$DTB" "$probe" >/dev/null 2>&1 || {
		echo "run-mtkshim-qemu.sh: superset tree lost $probe"; exit 1; }
done
fdtget -t s "$DTB" /chosen stdout-path | grep -q pl011 || {
	echo "run-mtkshim-qemu.sh: superset tree lost the PL011 stdout-path"; exit 1; }

fail=0
# The shim ends in a wfe halt (there is no stage1 in this payload, and on a phone you want the
# painted screen to stay up, not the phone to switch off). So the run is ended by watching for the
# shim's last line rather than by waiting out a timeout: a boot that takes 40 s is a failed boot
# pretending to be a slow one.
run_cpu() {   # run_cpu <cpu> -> writes /tmp/mtkshim-<cpu>.log, prints it
	local cpu=$1 log=/tmp/mtkshim-$cpu.log qpid i
	: > "$log"
	# shellcheck disable=SC2046
	qemu-system-aarch64 -M virt,gic-version=3 "${MACHINE[@]}" -dtb "$DTB" -cpu "$cpu" -accel tcg,tb-size=32 \
		-display none -serial "file:$log" -monitor none -no-reboot </dev/null >/dev/null 2>&1 &
	qpid=$!
	for i in $(seq 1 200); do                        # 20 s, in 100 ms steps
		grep -q "SHIM-END" "$log" 2>/dev/null && break
		kill -0 "$qpid" 2>/dev/null || break
		sleep 0.1
	done
	kill -TERM "$qpid" 2>/dev/null || true
	wait "$qpid" 2>/dev/null || true
	cat "$log"
}

want() {   # want <log> <pattern> <description>
	if ! grep -qE "$2" "$1"; then
		echo "FAIL ($1): expected $3"
		fail=$((fail + 1))
	fi
}

for cpu in cortex-a76 cortex-a710; do
	echo "=== $cpu ==="
	run_cpu "$cpu"
	log=/tmp/mtkshim-$cpu.log
	want "$log" "HakiOS MediaTek shim" "the shim to start"
	want "$log" "uart: (pl011|8250)" "a console to be found"
	want "$log" "memory: .*\+" "RAM to be read from the tree"
	want "$log" "psci: hvc v1" "PSCI to be read from the tree and called"
	want "$log" "payload: XNUP at" "the XNUP payload to be found"
	want "$log" "  'kernel' " "the payload's kernel entry to be listed"
	want "$log" "framebuffer: 50000000, 720x1640" "LK's atag framebuffer plus fb= geometry"
	want "$log" "SHIM-END" "the shim to reach its end"
	echo
done

# the two CPUs must disagree about pointer authentication, or the probe is not measuring anything
if grep -q "FEAT_PAuth ABSENT" /tmp/mtkshim-cortex-a76.log; then :; else
	echo "FAIL cortex-a76 should report FEAT_PAuth absent"; fail=$((fail + 1)); fi
if grep -q "register forms trapped 4 of 4" /tmp/mtkshim-cortex-a76.log; then :; else
	echo "FAIL cortex-a76 should trap all 4 register forms"; fail=$((fail + 1)); fi
if grep -q "FEAT_PAuth present" /tmp/mtkshim-cortex-a710.log; then :; else
	echo "FAIL cortex-a710 should report FEAT_PAuth present"; fail=$((fail + 1)); fi
if grep -q "register forms trapped 0 of 4" /tmp/mtkshim-cortex-a710.log; then :; else
	echo "FAIL cortex-a710 should trap none of the register forms"; fail=$((fail + 1)); fi

if [ "$fail" -ne 0 ]; then
	echo "run-mtkshim-qemu.sh: $fail failure(s)"
	exit 1
fi
echo "run-mtkshim-qemu.sh: ok"
echo "  This proves the shim's device-tree path on a CPU of each class. It proves nothing about"
echo "  MediaTek registers: QEMU has no MT6769. That is what the phone run is for."
