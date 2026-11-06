#!/bin/bash
# Build the HakiOS MediaTek boot shim as a Linux arm64 "Image" that LK (or QEMU -kernel) can start.
#   mtkshim/build.sh        -> mtkshim/shim.bin (the Image), mtkshim/shim.elf (for symbols)
# Needs an AArch64 GCC: aarch64-linux-gnu-gcc (Debian: gcc-aarch64-linux-gnu) or CROSS=<prefix>.
set -e
cd "$(dirname "$0")"
CROSS=${CROSS:-aarch64-linux-gnu-}
command -v ${CROSS}gcc >/dev/null || { echo "build.sh: no ${CROSS}gcc (apt install gcc-aarch64-linux-gnu, or CROSS=...)"; exit 1; }
[ -f font5x7.h ] || python3 mkfont.py font5x7.h
CFLAGS="-ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fno-pic -fpie -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11"
${CROSS}gcc $CFLAGS -c start.S -o start.o
${CROSS}gcc $CFLAGS -c shim.c -o shim.o
${CROSS}gcc -nostdlib -static-pie -Wl,--no-dynamic-linker -Wl,-T,linker.ld -Wl,--build-id=none -Wl,-z,notext start.o shim.o -o shim.elf
${CROSS}objcopy -O binary shim.elf shim.bin
# the image header's image_size is what LK keeps free after the load address; show it next to the file
SZ=$(${CROSS}nm shim.elf | awk '$3=="_image_size"{print $1}')
echo "shim.bin: $(stat -c %s shim.bin) bytes on disk, image_size 0x$SZ in memory"
