# Device: Redmi 14C 4G (MT6769Z, Helio G81 Ultra)

The phone this package is written for. It is **not** a working port. It is the honest starting
point, the tools that gather what a port needs from *your* phone, and a status matrix that says of
every subsystem whether it is done, blocked, or impossible.

| What | Where |
|---|---|
| SoC | MediaTek MT6769Z (Helio G81 Ultra): 2x Cortex-A75 + 6x Cortex-A55, **ARMv8.2, no FEAT_PAuth** |
| GPU | ARM Mali-G52 MC1 — **not Adreno**. Every GPU path in this tree targets Adreno. |
| Boot chain | ARM Trusted Firmware → LK (Little Kernel) → Linux. **No UEFI anywhere.** |
| Firmware | **not in this repository**: `pull-firmware.sh` pulls it off your phone over `adb` |
| SoC values | `pull-soc.sh` → `mtk.args` (from your phone's own device tree) |
| Boot image | `mkboot.sh` packs the shim + payload as an Android boot image |

## Read this before anything else

`docs/mediatek.md` explains the boot chain and every subsystem's prerequisites. Two facts decide
what is possible, and neither is a matter of effort:

1. **There is no UEFI on this chip.** `xnuloader/loader.c` is a UEFI application; Mu-Silicium does
   not exist for MediaTek. So the loader cannot run as it is. `mtkshim/` is what replaces the part
   of UEFI the loader actually uses, and `devices/redmi-14c/` is how it reaches the phone.
2. **This chip has no pointer authentication.** iOS 17 and later ship only arm64e user space, which
   needs FEAT_PAuth. The Helio G81 is ARMv8.2: the register forms of every pointer-auth instruction
   trap. `tools/pac-emu/` measures and emulates them; `docs/mediatek.md` states the cost and the
   three possible routes. **iOS 26's user space cannot run natively here.**

## Status

| Part | Status | What it needs |
|---|---|---|
| Boot shim (`mtkshim/`) | **runs**: verified under QEMU virt on cortex-a76 (no PAC) and cortex-a710 (PAC) | a phone run: `mkboot.sh` + `fastboot boot` |
| Device tree reader | **done**: `tools/mtk-soc-args.py`, tested against a synthetic MT6769 tree | your phone's own tree via `pull-soc.sh` |
| XNU kernel | **not built for this SoC** | an arm64 build for MT6769; XNU's platform layer is Apple-specific |
| UEFI replacement (stage 1) | **scaffolded**: `mtkshim/bootinfo.h` is the contract | port the 34 boot-service calls listed in `docs/mediatek.md` |
| Display | **blocked**: Mali-G52 has no driver here | a Mali (panfrost/panvk) kernel driver and userspace |
| GPU / Metal | **impossible as-is**: no Metal on Mali | a Metal→Vulkan layer with a Vulkan driver for G52, which does not exist today |
| Storage (UFS/eMMC) | **not started** | an MT6769 storage driver |
| Camera | **not started** | MT6769 CCI/seninf drivers and your sensor's tuning data — see below |
| Modem / calls | **not started** | MT6769 modem bring-up; no MediaTek equivalent of the POCO's path |
| Wi-Fi / BT | **not started** | MT7668/MT7663 class connectivity, entirely unlike the POCO's ath11k |
| Charging / fuel gauge | **not started** | MT6357 PMIC driver |
| iOS 26 user space (arm64e) | **impossible natively**; emulation is the only route | `tools/pac-emu/`, and the cost in `docs/mediatek.md` |
| iOS 16.5 user space (arm64) | **the realistic target** | what HakiOS already boots on the POCO; no PAC needed |
| Updates | **not Apple's**: see `docs/updates.md` | our own channel for our own artifacts (`tools/update-check.sh`) |

### The camera request, answered plainly

You asked for the camera to be reverse engineered so it beats Android. I am not going to claim that
is one step away, because it is not, and a claim like that costs you weeks. What is true:

* The Redmi 14C's cameras hang off MediaTek's CCI (camera control interface) and seninf block. The
  sensor is likely a Samsung S5K/OV part at a specific I2C address with vendor tuning data in
  `/vendor/lib64/camera/`.
* Those tuning files are **yours** to read from your phone (`pull-firmware.sh` collects them), and
  using them on your own hardware is normal.
* "Better than Android" is not a driver property. Android's camera quality comes from the vendor's
  3A (auto-exposure/focus/white-balance) algorithms and its tuning blobs. A HakiOS camera driver
  would have to reach feature parity *first* — which is months of work — before "better" is even a
  meaningful comparison.
* What is genuinely achievable and worth doing: a **raw capture path** (sensor → CSI → memory) with
  no vendor post-processing, which gives full control of the image pipeline. That is a real project
  with real value, and it is not the same thing as beating the stock camera app.

## Running

```sh
# 1. what your phone actually has (phone in Android, adb working)
devices/redmi-14c/pull-soc.sh          # -> mtk.args: console, RAM, reserved regions, framebuffer
devices/redmi-14c/pull-firmware.sh     # -> fw-mtk-*/: the files a driver will need (never committed)

# 2. build the shim and the test payload
mtkshim/build.sh
python3 xnuloader/mkpayload.py -o mtkshim/payload.bin kernel=prebuilt/mach.development.bcm2837 \
        ramdisk=prebuilt/userland/hello --cmdline "cpus=1"

# 3. the same binary, under QEMU, with a phone-shaped tree: the regression test
tools/test/run-mtkshim-qemu.sh

# 4. the phone (unlocked bootloader; nothing is flashed -- fastboot boot only)
devices/redmi-14c/mkboot.sh            # -> boot.img
fastboot boot devices/redmi-14c/boot.img
```

## Why `fastboot boot` and not flashing

HakiOS does not flash anything, on any device. `fastboot boot` puts the image in RAM and starts it;
a reboot returns the phone to Android with nothing written. Anything else would need a bootloader
that can be recovered if the image is wrong, and nobody has reverse engineered the Redmi 14C's boot
chain far enough to promise that.
