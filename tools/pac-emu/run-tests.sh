#!/bin/bash
# Build and run the PAC emulator tests on the host. Needs only gcc and binutils' AArch64 cross
# assembler (binutils-aarch64-linux-gnu); no phone, no kernel, no QEMU.
#   tools/pac-emu/run-tests.sh [--regen]     --regen rebuilds pac_decoder.h from the assembler first
set -e
cd "$(dirname "$0")"
CROSS=${CROSS:-aarch64-linux-gnu-}
command -v ${CROSS}as >/dev/null || { echo "run-tests.sh: no ${CROSS}as (apt install binutils-aarch64-linux-gnu)"; exit 1; }
command -v gcc >/dev/null || { echo "run-tests.sh: no gcc"; exit 1; }
if [ "$1" = "--regen" ] || [ ! -f pac_decoder.h ]; then
	python3 gen_encodings.py pac_decoder.h
fi
gcc -O1 -Wall -Wextra -Wno-unused-parameter -std=gnu11 -o /tmp/pac_emu_test \
	test_pac_emu.c pac_emu.c
exec /tmp/pac_emu_test
