# HakiOS: one Makefile at the top, the way an Android tree has one. Every target is a script
# below it; run the script itself for its options.
#
#   make                 help
#   make fetch           the OS files from the iOS 16.5 IPSW into sysimage/ (sysimage/fetch.sh)
#   make images          userland/root.apfs and userland/system.apfs (userland/build.sh)
#   make kernel          the kernel from source (xnu/apply.sh, xnu-deps/setup.sh, xnu-build.sh)
#   make loader          the boot loader, with identity.h made from your IPSW (xnuloader/build.sh)
#   make firmware        the phone's firmware off the phone over adb (devices/$(DEVICE)/pull-firmware.sh)
#   make qemu            a 20-minute session in QEMU (needs the iOS images)
#   make qemu-open       boot the open kernel in QEMU with NO Apple software
#                        (kernel + the hello RAM disk + a placeholder loader)
#   make phone           a session on the phone, then back to Android (tools/modem-cycle.sh)
#   make prebuilt        refresh prebuilt/ from what was built (tools/update-prebuilt.sh)
#   make mtkshim         the MediaTek boot shim, a Linux Image LK can start (mtkshim/build.sh)
#   make bootimg         devices/redmi-14c/boot.img: shim + payload (devices/redmi-14c/mkboot.sh)
#   make test            every test that needs no phone and no Apple software
#   make check           the whole CI check, as ci/github-check.yml runs it
#   make update-check    is a newer HakiOS published? (tools/update-check.sh)
#   make release         a release manifest for this tree (tools/release.py)
#   make clean           build output of the kernel, loader and images
#
#   DEVICE=poco-x5-pro   the device directory under devices/ (default poco-x5-pro)
#   ARGS="..."           kernel arguments for make phone (default: everything the phone has)
#   SECS=600             the session's length for make phone / make qemu
#   JOBS=8               parallel jobs for the kernel build
#   VERSION=0.4.0        the version make release writes into the manifest

DEVICE ?= poco-x5-pro
JOBS ?= 8
SECS ?= 600
ARGS_OPEN ?= cpus=1
ARGS ?= touchscreen=1 gpu=1 adsp=1 sound=4 wifi=1 haptic=5 modem=1 modem_again=1 modem_buffer=0xa8000000
IMAGE_OPTS ?= REST=1

.PHONY: help fetch images kernel loader loader-open hello qemu qemu-open phone prebuilt clean all \
        mtkshim bootimg test check update-check release

help:
	@sed -n '2,27p' Makefile | sed 's/^# \{0,1\}//'

all: kernel loader images

fetch:
	sysimage/fetch.sh

images:
	$(IMAGE_OPTS) userland/build.sh

kernel:
	[ -d xnu-src/xnu-8796.121.2 ] || xnu/apply.sh
	[ -x xnu-deps/host/bin/xnu-clang ] || xnu-deps/setup.sh
	./xnu-build.sh -j$(JOBS) -k

loader:
	xnuloader/build.sh

firmware:
	devices/$(DEVICE)/pull-firmware.sh

qemu:
	mkdir -p logs
	qemu/run-ios.sh $(SECS) -display gtk | tee logs/qemu.log

# The Apple-free path: no IPSW, no iOS user space. The loader is built with a placeholder identity
# (xnuloader/mkidentity.sh --placeholder), and the kernel boots the raw-syscall `hello` RAM disk.
# This is the configuration an outside contributor can build and run with nothing of Apple's.
loader-open:
	PLACEHOLDER=1 xnuloader/mkidentity.sh
	xnuloader/build.sh

# `hello` on its own: a static, raw-syscall program (no iOS SDK, no Apple libraries).
hello:
	. ./xnu-env.sh && xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding \
		-fno-stack-protector -fno-builtin -Wl,-e,_start -Wl,-pagezero_size,0x100000000 \
		-o userland/hello userland/src/hello.c

qemu-open: kernel loader-open hello
	mkdir -p logs
	xnuloader/mkpayload.py -o qemu/payload.bin \
		kernel=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
		ramdisk=userland/hello --cmdline "$(ARGS_OPEN)"
	qemu/run.sh $(SECS) | tee logs/qemu-open.log

phone:
	XL_SB_DELAY=8 tools/modem-cycle.sh "$(ARGS)" $(SECS)

prebuilt:
	tools/update-prebuilt.sh

# The MediaTek boot shim: a Linux arm64 Image, which is what LK on a MediaTek phone can start.
mtkshim:
	mtkshim/build.sh

# The Redmi 14C's boot image: the shim plus an XNUP payload. Nothing is flashed; see
# devices/redmi-14c/README.md for why `fastboot boot` and not a flash.
bootimg: mtkshim
	devices/redmi-14c/mkboot.sh

# Everything that can be checked without a phone and without any Apple software. This is the list
# CI runs (ci/github-check.yml); keeping it in one place means a test added here is a test in CI.
test:
	@echo "--- PAC emulator (the instruction forms an ARMv8.2 core traps on) ---"
	tools/pac-emu/run-tests.sh
	@echo "--- MediaTek device tree reader ---"
	sh tools/mtk-soc-args-test.sh
	@echo "--- reverse engineering: register map recovered from a known driver ---"
	tools/re-triage/run-tests.sh
	@echo "--- release / update machinery ---"
	tools/test/run-release-test.sh
	@echo "--- the shim under QEMU, on a CPU of each class ---"
	tools/test/run-mtkshim-qemu.sh

# The whole check, as ci/github-check.yml does it. `make test` needs the AArch64 cross tools and
# QEMU, which a bare runner may not have, so check keeps to what always works.
check:
	python3 -m py_compile $$(git ls-files '*.py')
	@for f in $$(git ls-files '*.sh' xl); do bash -n "$$f" || exit 1; done
	@echo "shell scripts parse"
	sh tools/soc-args-test.sh
	sh tools/mtk-soc-args-test.sh
	tools/re-triage/run-tests.sh
	tools/test/run-release-test.sh
	@echo "--- what CI cannot run, and why ---"
	@echo "  the shim needs aarch64-linux-gnu-gcc and qemu-system-aarch64: make test"

# Is a newer HakiOS published? Not "is there a new iOS": only Apple can authorise an install of iOS
# onto an Apple device, so nothing here can ask for one. See docs/updates.md.
update-check:
	tools/update-check.sh

# Describe this tree as a release: version, every artifact, and its SHA-256.
release:
	python3 tools/release.py manifest --version "$(or $(VERSION),$(shell git describe --tags --always --dirty 2>/dev/null || echo 0.0.0))" -o release.json
	python3 tools/release.py show release.json

clean:
	rm -rf xnu-src/xnu-8796.121.2/BUILD xnuloader/loader.obj xnuloader/loader.efi userland/root.apfs userland/system.apfs userland/obj
	rm -rf mtkshim/*.o mtkshim/shim.elf mtkshim/shim.bin mtkshim/font5x7.h devices/redmi-14c/boot.img
