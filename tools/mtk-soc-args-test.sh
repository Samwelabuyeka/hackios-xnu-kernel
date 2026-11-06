#!/bin/bash
# Regression test for tools/mtk-soc-args.py against a synthetic MT6769 tree (tools/test/mk-mt6769-dts.py).
# Checks the values the tool must produce, not just that it runs: a parser that reads the wrong cell,
# or that looks up the root as '' instead of '/', still "runs" while printing nothing useful.
# Wired into ci/github-check.yml.
set -e
cd "$(dirname "$0")/.."
command -v dtc >/dev/null || { echo "mtk-soc-args-test.sh: no dtc (apt install device-tree-compiler)"; exit 1; }
dtb=$(python3 tools/test/mk-mt6769-dts.py /tmp/mtk-test.dtb)
out=$(python3 tools/mtk-soc-args.py "$dtb" 2>/dev/null)
fail=0

want() {   # want <expected line>
	if ! printf '%s\n' "$out" | grep -qxF "$1"; then
		echo "FAIL expected: $1"
		fail=$((fail + 1))
	fi
}

want "mtk_uart=0x11002000"
want "mtk_uart_shift=2"
want "mtk_mem=0x40000000+0x80000000"
want "mtk_mem=0x100000000+0x40000000"
want "mtk_rsv=atf-reserved-memory@43000000:0x43000000+0x300000"
want "mtk_rsv=tee-reserved-memory@54600000:0x54600000+0x500000"

# the report must name the device, which is what proves the root node was parsed at all
if ! python3 tools/mtk-soc-args.py "$dtb" 2>&1 >/dev/null | grep -qF "Synthetic MT6769"; then
	echo "FAIL the root node's model was not read"
	fail=$((fail + 1))
fi

if [ "$fail" -ne 0 ]; then
	echo "mtk-soc-args-test.sh: $fail failure(s)"
	exit 1
fi
echo "mtk-soc-args-test.sh: ok ($(printf '%s\n' "$out" | wc -l) values)"
