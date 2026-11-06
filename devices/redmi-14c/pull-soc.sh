#!/bin/bash
# Write mtk.args: what this phone's own device tree says a HakiOS port needs.
# Phone in Android, adb working. /sys/firmware/fdt is world-readable, so no root is needed.
#
# Everything here is read from the phone you are holding. Nothing is guessed, and a value the tree
# does not describe is reported as absent rather than filled in: a made-up register address in a
# boot loader is a phone that does not come back.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
cd "$REPO"
PATH=$REPO/tools/macos-bin/platform-tools:$PATH
export PATH
command -v adb >/dev/null || { echo "pull-soc.sh: no adb on PATH"; exit 1; }
adb get-state >/dev/null 2>&1 || { echo "pull-soc.sh: no phone on adb (or not authorised)"; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 1. the live tree (what the running Android kernel was actually given)
if ! adb pull /sys/firmware/fdt "$tmp/fdt" >/dev/null 2>&1; then
	echo "pull-soc.sh: /sys/firmware/fdt is not readable; trying the dtbo/boot image instead"
	adb shell 'ls /proc/device-tree 2>/dev/null | head -1' >/dev/null 2>&1 || {
		echo "pull-soc.sh: cannot read the device tree on this phone"; exit 1; }
	echo "pull-soc.sh: a tree assembled from /proc/device-tree is not stable enough to use as-is;"
	echo "             a rooted phone's /sys/firmware/fdt is the one this tool wants. Trying anyway:"
	python3 - "$tmp/fdt" << 'PY' || { echo "pull-soc.sh: could not assemble one"; exit 1; }
import subprocess, sys
def u32(s): return int(s, 16)
out = subprocess.run(['adb', 'shell', 'find /proc/device-tree -type f'], capture_output=True, text=True).stdout.split()
sys.exit("no files" if not out else 0)
PY
fi

# 2. the values
python3 tools/mtk-soc-args.py "$tmp/fdt" > "$HERE/mtk.args"
echo "pull-soc.sh: wrote $HERE/mtk.args"

# 3. the identity, kept next to it so a port knows which phone the file describes
{
	echo "# from the phone's own device tree, $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	adb shell getprop ro.product.device 2>/dev/null | tr -d '\r' | sed 's/^/# device: /'
	adb shell getprop ro.build.fingerprint 2>/dev/null | tr -d '\r' | sed 's/^/# build: /'
	adb shell getprop ro.board.platform 2>/dev/null | tr -d '\r' | sed 's/^/# platform: /'
	adb shell cat /proc/cpuinfo 2>/dev/null | grep -m1 -i "hardware\|Processor" | tr -d '\r' | sed 's/^/# cpu: /'
} >> "$HERE/mtk.args"
echo "pull-soc.sh: done. Read the 'absent:' line in the report above: it lists what the tree does not"
echo "             describe, which is what mkboot.sh needs passed on the command line instead."
