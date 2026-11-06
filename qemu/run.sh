#!/bin/bash
# Boot the XNU loader + kernel payload under QEMU (virt, GICv3, UEFI at EL1).
# usage: run.sh [seconds] [extra qemu args...]
# env: QEMU_TB_MB           megabytes for QEMU's cache of translated code (default 384; QEMU's own
#                           default of 1 GB mostly costs host memory)
#      QEMU_SMP             processor cores of the machine (default 1); the kernel uses as many of them as
#                           its boot-arg cpus=N says (CMDLINE="cpus=4" for run-ios.sh), one without it
#      QEMU_EFI, QEMU_VARS  the UEFI firmware for aarch64 guests; found on its own where Arch's
#                           edk2-aarch64 or Debian/Ubuntu's qemu-efi-aarch64 (AAVMF) put it
#      QEMU_MEM             the guest's RAM (default 6G)
#      QEMU_RAM_FILE=1      back the guest's RAM with a sparse file (qemu/ram.img) instead of anonymous
#                           memory. The guest touches only what the kernel uses, so a 6 GB guest then
#                           starts on a host with 1 GB: the way the loader and kernel were brought up on a
#                           2-core, 985 MB sandbox. Slower when the guest does page a lot; for a desktop
#                           leave it off.
cd "$(dirname "$0")"
T=${1:-20}; shift
# the UEFI firmware: the first of the given / Arch / Debian locations that exists, with its variable store
for c in "$QEMU_EFI" /usr/share/edk2/aarch64/QEMU_EFI.fd /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd; do
	[ -n "$c" ] && [ -f "$c" ] && { EFI=$c; break; }
done
[ -n "$EFI" ] || { echo "run.sh: no UEFI firmware found (install edk2-aarch64 / qemu-efi-aarch64, or set QEMU_EFI and QEMU_VARS)"; exit 1; }
VARS=${QEMU_VARS:-$(dirname "$EFI")/$(basename "$EFI" | sed -e 's/QEMU_EFI/QEMU_VARS/' -e 's/_CODE/_VARS/')}
cp "$VARS" vars.fd 2>/dev/null || { echo "run.sh: no UEFI variable store at $VARS (set QEMU_VARS)"; exit 1; }
# the loader is the boot application of the little FAT volume QEMU makes of esp/:
# the one built here, or else the one that comes with the repository
L=../xnuloader/loader.efi; [ -f $L ] || L=../prebuilt/loader.efi
mkdir -p esp/EFI/BOOT && cp -u $L esp/EFI/BOOT/BOOTAA64.EFI
MEM=${QEMU_MEM:-6G}
RAM=()
if [ -n "$QEMU_RAM_FILE" ]; then
	rm -f ram.img
	RAM=(-object "memory-backend-file,id=ram,size=$MEM,mem-path=$PWD/ram.img,share=on,prealloc=off" -machine memory-backend=ram)
fi
timeout "$T" qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp ${QEMU_SMP:-1} -m $MEM \
  -accel tcg,tb-size=${QEMU_TB_MB:-384} \
  -drive if=pflash,format=raw,readonly=on,file="$EFI" \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive file=fat:rw:esp,format=raw,if=virtio \
  -device loader,file=payload.bin,addr=0xF0000000,force-raw=on \
  "${RAM[@]}" \
  -display none -serial stdio -monitor none -no-reboot "$@" 2>&1
