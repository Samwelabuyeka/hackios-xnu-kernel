#!/bin/bash
# Pull off your phone the vendor files a MediaTek port will eventually need, into fw-mtk-*/ at the
# top of the repository. Nothing here is committed: it is MediaTek's and Xiaomi's, and every user
# pulls their own copy from their own hardware (LEGAL.md).
#
# Phone in Android, adb working, bootloader unlocked for the parts under /vendor/firmware_mnt.
# Every path is tried and reported; a file this phone does not have is not an error.
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
PATH=$REPO/tools/macos-bin/platform-tools:$PATH
export PATH
command -v adb >/dev/null || { echo "pull-firmware.sh: no adb on PATH"; exit 1; }
adb get-state >/dev/null 2>&1 || { echo "pull-firmware.sh: no phone on adb"; exit 1; }

pull() {   # pull DIR SOURCE...
	local dir=$1; shift
	mkdir -p "$dir"
	local got=0
	for f in "$@"; do
		if adb pull "$f" "$dir/" >/dev/null 2>&1; then got=$((got + 1)); fi
	done
	echo "pull-firmware.sh: $dir: $got of $# file(s)"
	[ "$got" -eq 0 ] && rmdir "$dir" 2>/dev/null || true
}

# --- what tells a port which files this phone actually has ------------------------------------
# The vendor's own manifest lists every partition image and firmware file with its path. Kept as a
# text file so a driver author can grep it without the phone plugged in.
mkdir -p fw-mtk-manifest
adb shell 'ls -l /vendor/etc/vintf/manifest.xml /vendor/etc/vintf/manifest_*.xml 2>/dev/null' \
	> fw-mtk-manifest/vintf-manifest-list.txt 2>/dev/null || true
for f in $(adb shell 'ls /vendor/etc/vintf/manifest*.xml 2>/dev/null' | tr -d '\r'); do
	adb pull "$f" fw-mtk-manifest/ >/dev/null 2>&1 || true
done
echo "pull-firmware.sh: fw-mtk-manifest: $(ls -1 fw-mtk-manifest 2>/dev/null | wc -l | tr -d ' ') file(s)"

# --- connectivity (a MediaTek MT76xx part, not Qualcomm's ath11k) -----------------------------
pull fw-mtk-connsys \
	/vendor/firmware/WIFI_RAM_CODE_6769.bin /vendor/firmware/WIFI_RAM_CODE_6769_1.bin \
	/vendor/firmware/mt7668_patch_e1_hdr.bin /vendor/firmware/mt7668_patch_e2_hdr.bin \
	/vendor/firmware/ROMv2_lm_patch_1_0_hdr.bin /vendor/firmware/ROMv2_lm_patch_1_1_hdr.bin

# --- camera: the sensor tuning the vendor's 3A uses. Yours, on your hardware. -----------------
# The files are named per sensor module; this takes whatever the phone has rather than assuming a
# part number, and records the names so the driver author knows which sensor is fitted.
mkdir -p fw-mtk-camera
adb shell 'ls -l /vendor/lib64/camera/ 2>/dev/null' > fw-mtk-camera/camera-libs.txt 2>/dev/null || true
for f in $(adb shell 'ls /vendor/lib64/camera/*.bin /vendor/lib64/camera/*.xml 2>/dev/null' | tr -d '\r'); do
	adb pull "$f" fw-mtk-camera/ >/dev/null 2>&1 || true
done
echo "pull-firmware.sh: fw-mtk-camera: $(ls -1 fw-mtk-camera 2>/dev/null | wc -l | tr -d ' ') file(s)"
# and which sensors are on the I2C buses at all: this is the fact a camera driver starts from
adb shell 'cat /sys/bus/i2c/devices/*/name 2>/dev/null' > fw-mtk-camera/i2c-devices.txt 2>/dev/null || true
adb shell 'cat /proc/device-tree/soc/i2c@*/status 2>/dev/null' >/dev/null 2>&1 || true

# --- audio: MediaTek's ADSP runs the DSP firmware; the codec is a MT6357 PMIC part -------------
pull fw-mtk-audio \
	/vendor/firmware/aud_dsp_idle.bin /vendor/firmware/audio_dsp_hw_gain.bin

# --- the modem and the connectivity DSP, from the firmware partition --------------------------
pull fw-mtk-md \
	/vendor/firmware_mnt/image/md1img/modem.img \
	/vendor/firmware_mnt/image/md1img/dsp.img

# --- the GPU: Mali-G52 firmware, if this phone ships it separately from the driver ------------
pull fw-mtk-gpu /vendor/firmware/mali_kbase.bin /vendor/firmware/gpu_mt6769.bin

# --- the panel: LK's LCM table names the panel, and a display driver needs its timings ---------
adb shell 'cat /proc/device-tree/chosen/atag,videolfb-lcmname 2>/dev/null' \
	> fw-mtk-panel.txt 2>/dev/null || true
adb shell 'dumpsys SurfaceFlinger 2>/dev/null | grep -m3 -i "display\|resolution"' \
	>> fw-mtk-panel.txt 2>/dev/null || true

echo
echo "pull-firmware.sh: done."
echo "  Nothing here is tracked by git (see .gitignore): it is the vendor's, and each user pulls"
echo "  their own. Do not commit it, and do not republish a build that contains it."
echo "  fw-mtk-camera/camera-libs.txt and i2c-devices.txt are the two files that tell you which"
echo "  sensors this phone actually has -- read them before writing a camera driver for a part"
echo "  number you assumed."
