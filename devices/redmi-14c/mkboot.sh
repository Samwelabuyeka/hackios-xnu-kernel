#!/bin/bash
# Pack mtkshim + the XNUP payload into an Android boot image this phone's LK will load.
#
# How it gets there: an MT6769 bootloader reads a boot image's kernel, puts it at the kernel load
# address, puts the ramdisk at a fixed offset, and jumps to the kernel with the device tree in x0.
# mtkshim/ is built to look exactly like a Linux kernel to that loader (see mtkshim/README.md), and
# the XNUP payload rides where the ramdisk would be -- which is also where the shim looks for it
# (/chosen/linux,initrd-start). No driver, no UEFI, nothing MediaTek-specific beyond the header.
#
# usage: mkboot.sh [--page-size N] [--base HEX] [--kernel-offset HEX] [--ramdisk-offset HEX]
#                   [--cmdline "..."] [-o OUT]
# Defaults are the ones MT6769's LK uses for a 4 KB-page image. If fastboot boot fails with
# "image too large" or the phone resets immediately, read the offsets off the phone's own stock
# boot image first: mkboot.sh --from-stock stock-boot.img shows them.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
cd "$REPO"

MKBOOTIMG=$REPO/devices/poco-x5-pro/prebuilt/mkbootimg.py
PAGESIZE=2048
BASE=0x40000000
KERNEL_OFFSET=0x00008000
RAMDISK_OFFSET=0x01000000
TAGS_OFFSET=0x00000100
CMDLINE=""
OUT=$HERE/boot.img
DTB=""
DUMP_STOCK=""
HEADER_VERSION=2

while [ $# -gt 0 ]; do
	case "$1" in
	--page-size)      PAGESIZE=$2; shift 2 ;;
	--base)           BASE=$2; shift 2 ;;
	--kernel-offset)  KERNEL_OFFSET=$2; shift 2 ;;
	--ramdisk-offset) RAMDISK_OFFSET=$2; shift 2 ;;
	--tags-offset)    TAGS_OFFSET=$2; shift 2 ;;
	--cmdline)        CMDLINE=$2; shift 2 ;;
	--dtb)            DTB=$2; shift 2 ;;
	--header-version) HEADER_VERSION=$2; shift 2 ;;
	-o|--output)      OUT=$2; shift 2 ;;
	--from-stock)     DUMP_STOCK=$2; shift 2 ;;
	-h|--help)        sed -n '2,20p' "$0"; exit 0 ;;
	*) echo "mkboot.sh: unknown option $1"; exit 1 ;;
	esac
done

# --- read a boot image's own header, so the phone's values can be copied rather than guessed ----
if [ -n "$DUMP_STOCK" ]; then
	python3 - "$DUMP_STOCK" << 'PY'
import struct, sys

# Boot image header v0 (Android through 9). Read the fields this script needs to reproduce, and
# print the values a caller should pass back in with --base/--kernel-offset/--ramdisk-offset.
d = open(sys.argv[1], 'rb').read()
if d[:8] != b'ANDROID!':
    sys.exit("not an Android boot image (no ANDROID! magic)")
kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, \
    second_size, second_addr, tags_addr, page_size = struct.unpack_from('<8I', d, 8)
ver = struct.unpack_from('<I', d, 40)[0]
cmdline = d[64:64 + 512].split(b'\x00', 1)[0].decode(errors='replace')
print(f"header version {ver}, page size {page_size}")
for n, v in (('kernel_size', kernel_size), ('kernel_addr', kernel_addr),
             ('ramdisk_size', ramdisk_size), ('ramdisk_addr', ramdisk_addr),
             ('second_size', second_size), ('tags_addr', tags_addr)):
    print(f"  {n:14} 0x{v:x}")
print(f"  cmdline        {cmdline!r}")
print()
print("Pass these to mkboot.sh:")
print(f"  --page-size {page_size} --base 0x{kernel_addr - 0x8000:x} "
      f"--kernel-offset 0x8000 --ramdisk-offset 0x{ramdisk_addr - (kernel_addr - 0x8000):x}")
PY
	exit 0
fi

# --- inputs -----------------------------------------------------------------------------------
SHIM=$REPO/mtkshim/shim.bin
PAYLOAD=${PAYLOAD:-$REPO/mtkshim/payload.bin}
# The phone's own device tree. pull-soc.sh keeps it as mtk.dtb, and it belongs in the boot image:
# LK on MediaTek hands the kernel whatever dtb the image carries, and the shim reads that tree for
# everything it knows. Without one, header v2 cannot be built at all -- mkbootimg refuses an empty
# dtb -- so fall back to header v0, which has no dtb field and is what older LK versions expect.
[ -n "$DTB" ] || { [ -f "$HERE/mtk.dtb" ] && DTB=$HERE/mtk.dtb; }
if [ -z "$DTB" ] && [ "$HEADER_VERSION" -ge 2 ]; then
	echo "mkboot.sh: no device tree. Header v2 needs one, and the shim wants the phone's own."
	echo "            Run devices/redmi-14c/pull-soc.sh (it keeps mtk.dtb), or pass --dtb FILE."
	echo "            Falling back to header v0, which carries no dtb; if the phone's LK expects"
	echo "            v2 it will reject the image and say so."
	HEADER_VERSION=0
fi
[ -f "$SHIM" ] || { echo "mkboot.sh: no $SHIM -- run mtkshim/build.sh"; exit 1; }
[ -f "$PAYLOAD" ] || { echo "mkboot.sh: no $PAYLOAD -- build one with xnuloader/mkpayload.py (see README)"; exit 1; }

# the shim writes these on its own framebuffer before anything else runs, so pass the geometry when
# the phone's tree does not carry it
if [ -z "$CMDLINE" ] && [ -f "$HERE/mtk.args" ]; then
	CMDLINE=$(grep -E '^mtk_fb_geom=' "$HERE/mtk.args" | sed 's/^mtk_fb_geom=/fb=/')
fi

# --- pack -------------------------------------------------------------------------------------
# The shim goes in as the "kernel": LK does not care that it is not Linux, it jumps to the entry
# point in the arm64 Image header. The XNUP payload goes in as the "ramdisk", which is what LK
# loads to the ramdisk offset and what the shim looks for in the tree.
DTBARG=()
[ -n "$DTB" ] && DTBARG=(--dtb "$DTB")
python3 "$MKBOOTIMG" --kernel "$SHIM" --ramdisk "$PAYLOAD" "${DTBARG[@]}" \
	--base "$BASE" --kernel_offset "$KERNEL_OFFSET" --ramdisk_offset "$RAMDISK_OFFSET" \
	--tags_offset "$TAGS_OFFSET" --pagesize "$PAGESIZE" \
	--cmdline "$CMDLINE" --header_version "$HEADER_VERSION" -o "$OUT"

echo "mkboot.sh: $OUT ($(stat -c %s "$OUT") bytes)"
echo "  shim    $(stat -c %s "$SHIM") bytes at $KERNEL_OFFSET"
echo "  payload $(stat -c %s "$PAYLOAD") bytes at $RAMDISK_OFFSET"
echo "  dtb     ${DTB:-(none: header v$HEADER_VERSION)}"
echo "  cmdline '${CMDLINE}'"
echo "  header  v$HEADER_VERSION"
echo
echo "  fastboot boot $OUT"
echo "  (nothing is flashed; a reboot returns the phone to Android)"
