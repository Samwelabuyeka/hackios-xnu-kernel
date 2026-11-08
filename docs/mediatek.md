# MediaTek: what this tree can and cannot do on an MT6769

This is the document `devices/redmi-14c/README.md` points at. It exists because the Redmi 14C
(MT6769Z, Helio G81 Ultra) is not a Qualcomm phone with a different name on the box: three things
about it break assumptions the rest of this tree makes, and each one has a different answer.

The short version:

| Assumption this tree was built on | True on the Redmi 14C? | Answer |
|---|---|---|
| the boot chain is UEFI, so a UEFI loader can run | **no** — ARM Trusted Firmware → LK → Linux | `mtkshim/` (stage 0), `bootinfo.h` (the contract) |
| the GPU is Adreno, so freedreno/Turnip can drive it | **no** — Mali-G52 | nothing in this tree; needs panfrost/panvk |
| user space is arm64e, which needs FEAT_PAuth | **no** — ARMv8.2, no PAC | run arm64 (iOS 16.5 class), or emulate PAC |

Nothing below is a promise. Where something is not done it says "not done".

---

## 1. The boot chain, and why the loader cannot simply be ported

The POCO X5 Pro boots like a PC: firmware starts a UEFI application, and `xnuloader/loader.c` is
that application. It calls into the UEFI boot services directly — 34 call sites, 12 distinct
services. On MediaTek there is no UEFI. The chain is:

```
BootROM → preloader → ARM Trusted Firmware (BL1/BL2/BL31) → LK (Little Kernel) → Linux
```

LK boots a Linux kernel and nothing else. It does not know what an EFI application is. So the
choice is not "port the loader"; it is "write the thing LK *can* start, and let it rebuild the part
of UEFI the loader was using".

That thing is `mtkshim/`, and it is a valid Linux arm64 `Image`:

| Header field | value | why |
|---|---|---|
| `code0` | `b _start` | LK jumps to the load address + 0 |
| `text_offset` | `0x80000` | so LK's usual load address works unchanged |
| `flags` | `0xa` | bit 1 = little-endian, bit 3 = physical placement, no KASLR |
| magic | `ARM\x64` | the only check LK makes |

Everything the shim learns is read out of the device tree — console, RAM, reserved regions, the
framebuffer LK left lit, the XNUP payload, PSCI — and written into `struct bootinfo`
(`mtkshim/bootinfo.h`), which is the contract between stage 0 and whatever comes after it. That is
deliberate: a shim that read MediaTek registers directly would be untestable without the phone, and
would be a different binary under QEMU than on the device. This way the binary under test is the
binary that ships.

### What each UEFI call becomes

The loader's 34 boot-service call sites, and what each one turns into when the caller is stage 1
behind `struct bootinfo` instead of a UEFI application:

| UEFI call | sites | what the loader uses it for | on MediaTek |
|---|---|---|---|
| `handle_protocol` | 11 | get the protocol interface behind a handle | nothing to get: LK already loaded the payload, and its address is in `/chosen/linux,initrd-start` |
| `locate_handle_buffer` | 5 | enumerate `BlockIo` / `PartitionInfo` / `DevicePath` handles to find the partition holding the payload | same — no block device is involved; `mkboot.sh` puts the XNUP payload in the boot image as the ramdisk |
| `locate_protocol` | 5 | charger (QCOM charger-ex), CPU arch (exception hook), USB, GOP | MT6357 PMIC over SPMI/I2C; MTK exception vectors (`pac_vectors` in `start.S` is the start of it); MTK USB; the LCM/LK framebuffer |
| `get_memory_map` | 3 | find a physical window for the kernel; print the map | `/memory` and `/reserved-memory` from the tree, into `BI.mem[]` / `BI.rsv[]` |
| `allocate_pages` | 2 | claim that window; allocate ramfb | carve from a reserved region; there is no ramfb outside QEMU |
| `create_event`, `create_event_ex`, `check_event`, `signal_event`, `close_event` | 6 | wait for a disk or USB device to appear | nothing to wait for |
| `stall` | 1 | sleep while polling USB | `cntfrq_el0` and a busy-wait; it is already in `BI.cntfrq` |
| `set_watchdog_timer(0,…)` | 1 | turn off the firmware watchdog | LK/ATF watchdog, via PSCI or the MTK WDT; PSCI is already probed into `BI.psci_version` |

Read the table as a dependency list, not a progress list. The first three rows are the ones that
cost work, and only the third row involves talking to MediaTek hardware at all.

### Stage 0 is done and tested; stage 1 is scaffolded

`mtkshim/` is done and runs: under QEMU's virt machine it finds the PL011, reads RAM, reads LK's
`atag,videolfb-*` framebuffer, finds the XNUP payload, calls PSCI, and halts with `SHIM-END`
(`tools/test/run-mtkshim-qemu.sh`, green on cortex-a76 and cortex-a710). On the phone it has **not**
run — that is one `fastboot boot` away, and nothing is flashed.

Stage 1 — a program that consumes `struct bootinfo` and does the driver work — is scaffolded only.
`bootinfo.h` is deliberately laid out so it can be a freestanding program, an ELF, or the start of
the XNU platform layer. It does not exist yet.

---

## 2. The chip's architecture, and the pointer-authentication problem

This is the finding the whole iOS 26 question turns on, and it is a property of the silicon, not of
the port's effort.

| | Helio G81 (this phone) | Snapdragon 8 Elite (what `dada` targets) |
|---|---|---|
| cores | 2× Cortex-A75 + 6× Cortex-A55 | Oryon |
| architecture | ARMv8.2-A | ARMv9.x |
| FEAT_PAuth (ARMv8.3) | **absent** | present |

`ID_AA64ISAR1_EL1.APA/API/GPA/GPI` are all zero on this core, which the shim prints on the phone's
own UART:

```
pac: ID_AA64ISAR1 APA=0 API=0 GPA=0 GPI=0 ISAR2 APA3=0 -> FEAT_PAuth ABSENT
pac: hint forms run, register forms trapped 4 of 4
```

The second line is the part that matters. Pointer authentication has two encodings of each
operation:

* the **HINT-space** forms (`paciasp`, `autiasp`) occupy the space architecture already defined as
  no-ops, so an ARMv8.2 core executes them as nothing. Code built with them runs, unprotected.
* the **register** forms (`paciza x0, x1`, …) use real opcode space that ARMv8.2 does not define, so
  they raise an exception with `ESR_EL1.EC = 0x00` (unknown instruction) on this core.

The shim counts this directly rather than trusting a feature register: it executes one of each and
counts how many trapped. On the A75 class 4 of 4 trap; on an A710, 0 of 4. That is the evidence, and
`tools/pac-emu/` is the consequence:

* `tools/pac-emu/pac_decoder.h` — 33 forms, with **every operand bit position inferred from GNU as**
  rather than typed from memory. Doing it that way caught four errors that a hand-written table
  would have shipped: PACGA's `Rm` at 20:16, LDRAA's 10-bit sign-split immediate, PACIA signing `Xd`
  and not `Xn`, and XPACI testing bit 55 rather than the top byte.
* `tools/pac-emu/test_pac_emu.c` — 261 checks, 0 failures.

### The cost, measured rather than guessed

`test_pac_emu.c` assembles an Objective-C-dispatch-shaped loop and counts:

```
instruction mix: 22 instructions, 1 PAC, of which 1 trap on ARMv8.2
one trapping PAC instruction per 22.0 instructions
```

So on this core, roughly **4.5 % of executed instructions raise an exception** in arm64e code. An
emulator turns each of those into a trap entry, a decode, a key computation and a return — call it
two orders of magnitude more expensive than the instruction itself. That is not a slowdown of a few
percent; it is a different regime, and it is why "emulate PAC" is stated here as a route with a
measured price rather than as a workaround.

### The three routes

| Route | What it means | Cost | Status |
|---|---|---|---|
| **1. arm64, not arm64e** | run the iOS 16.5-class arm64 user space HakiOS already boots on the POCO. No PAC instructions exist, so nothing traps. | nothing extra; it is what the tree does today | **the realistic target** |
| **2. Emulate PAC** | trap handler + `tools/pac-emu`'s decoder in the kernel, keys derived per-task. arm64e user space then runs. | ~4.5 % of instructions trap; the emulation path is far more expensive than the instruction it replaces | emulator is built and tested; nothing is wired into a kernel |
| **3. Run on PAC silicon** | a phone whose cores have FEAT_PAuth (ARMv8.3+/ARMv9). This is what `superboyyy/hakios-arm64e-dada` assumes (`HAS_STD_PAC` for non-Apple ARMv8.3+). | a different phone | correct on the right hardware; **not this phone** |

Route 1 is what the Redmi 14C gets. Route 2 is the only way iOS 26's user space could run on this
silicon, and the emulator is the first piece of it, not the whole of it. Route 3 is not a workaround
for this phone at all — it is a statement about which phones the arm64e work applies to.

**So: iOS 26's arm64e user space cannot run natively on the Redmi 14C.** Not because the port is
young, but because the instructions it is compiled to do not exist on the chip.

---

## 3. The GPU, and why the `gpu2` path does not transfer

`superboyyy/hakios-arm64e-dada`'s `/gpu2` is Metal→Vulkan through a modified Mesa Turnip, which is
a **freedreno** driver: Adreno hardware, driven by the Adreno kernel driver (`msm`). The Redmi 14C
has a **Mali-G52 MC1**. None of that stack applies:

| | Adreno (POCO, `dada`) | Mali-G52 (Redmi 14C) |
|---|---|---|
| Mesa driver | freedreno / Turnip (Vulkan) | panfrost / panvk |
| kernel driver | `msm` | `panfrost` (or `kbase`, the vendor blob) |
| Metal via Vulkan | the `gpu2` approach | would need panvk to be complete first |

This tree's own GPU work (`docs/air.md`, `docs/metal.md`, `docs/air-report.md`) is AIR→NIR for ir3,
which is freedreno again. It is a real body of work and it is irrelevant to this phone. Display is
blocked before GPU is even reached: with no Mali kernel driver there is no DRM device, so there is
nothing for a compositor to draw on.

---

## 4. Subsystem prerequisites, in dependency order

Ordered the way they have to be built, because the later ones are unreachable without the earlier:

1. **Stage 0** — `mtkshim/`. Done; QEMU-tested; phone run pending.
2. **Stage 1** — consume `struct bootinfo`. Scaffolded (`bootinfo.h`).
2b. **Storage** — the payload has to come from somewhere on a real boot, and the system image has to
   be readable. MT6769 uses UFS or eMMC behind MediaTek's MSDC/UFShci. Not started.
3. **Display** — an LCM/DSI driver (MediaTek `disp`/`dsi`) plus a framebuffer, then a Mali kernel
   driver before anything accelerated. Not started; the shim can already paint on what LK left lit.
4. **Input** — the touch panel over I2C, and the MT6357 PMIC for keys. Not started.
5. **Wi-Fi/BT** — MT7668/MT7663-class connectivity (connsys), which shares nothing with the POCO's
   ath11k. `pull-firmware.sh` collects the blobs. Not started.
6. **Charging/fuel gauge** — MT6357 PMIC. Not started.
7. **Modem** — MediaTek's modem is a different architecture from the POCO's Hexagon MPSS/GLINK/QMI
   path in `xnu/files/bsd/dev/hakios.mpss.c`. Nothing in that file transfers. Not started.
8. **Camera** — the CCI/seninf block and a sensor driver plus vendor tuning. Not started. See
   `devices/redmi-14c/README.md` for why "better than Android" is the wrong goal to start from, and
   what the honest one is.

Every one of those is "not started", and saying so is the point of this document.

---

## 5. Reproducing all of it

```sh
mtkshim/build.sh                      # shim.bin: the Linux Image LK can start
tools/test/run-mtkshim-qemu.sh        # green on cortex-a76 (no PAC) and cortex-a710 (PAC)
tools/pac-emu/run-tests.sh            # 261 checks, 0 failures
sh tools/mtk-soc-args-test.sh         # the DTB reader against a synthetic MT6769 tree
devices/redmi-14c/mkboot.sh           # boot.img: shim + XNUP payload (+ mtk.dtb if present)
```

None of these touch a phone. The phone run is `fastboot boot devices/redmi-14c/boot.img`, and it
writes nothing.

### What QEMU proves, and what it cannot

QEMU has no MT6769. The virt machine proves the shim's device-tree path, the payload discovery, the
PSCI call, and that the PAC probe distinguishes an ARMv8.2 core from an ARMv9 one. It proves
**nothing** about any MediaTek register, any MediaTek clock, or whether LK on this particular phone
leaves the framebuffer in the shape the shim expects. Those are exactly the things the phone run is
for, and no test here should be read as covering them.
