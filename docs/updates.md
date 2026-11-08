# Updates: how HakiOS gets them, and the two things it will never do

The request behind this document was: *"find a way to ensure we get upgrades just like their users
do."* This answers it honestly, which means starting with what Apple's update mechanism actually is,
because "just like their users" is not achievable and pretending otherwise would waste your time.

## 1. What an iPhone update actually is

An iOS OTA is not a file you download and install. It is three separate locks:

| Lock | What holds it | Can HakiOS have it? |
|---|---|---|
| **Signature** | Apple signs the update image; the device verifies it against a key fused into the SoC | no — the key is Apple's, in Apple silicon |
| **Personalisation (TSS)** | Apple's signing server issues an *APTicket* bound to that device's **ECID** and a **nonce** | no — it is issued for Apple hardware running Apple's OS |
| **Install authorisation** | the bootloader only accepts an image whose APTicket matches, and the nonce can only be spent once | no — same reason |

All three collapse into one sentence: **only Apple can authorise an install of iOS onto an Apple
device.** That is the security model working as designed, not an obstacle to route around. There is
no "TSS workaround" for a non-Apple OS, and any project claiming one is either lying or describing
something else.

So HakiOS does not receive Apple's OTA, and this document does not promise it will. What HakiOS can
have — and what the rest of this document is about — is **its own update channel for its own files.**

## 2. What actually updates on HakiOS

HakiOS is a kernel, a loader, user-space programs and, on MediaTek, a boot shim. Those are files in
*this* repository, built from *this* source. They can be replaced by anything that can write them:
`fastboot boot` for a RAM-only run, or a flash of the boot partition if you choose to. The update
question is therefore "how does a HakiOS install learn that a newer set of its own files exists, and
know the set is intact and really ours?" — which is answerable.

| Artifact | Where it comes from | How it is delivered today |
|---|---|---|
| `mach.development.bcm2837` (kernel) | `xnu-build.sh` | in the XNUP payload, `fastboot boot`, or QEMU |
| `loader.efi` | `xnuloader/build.sh` | same |
| `userland/*` (our programs and libraries) | `userland/build.sh` | in `root.apfs` / `system.apfs` |
| `shim.bin` (MediaTek stage 0) | `mtkshim/build.sh` | in `boot.img` (`devices/redmi-14c/mkboot.sh`) |

## 3. The channel

`tools/update-check.sh` is the whole mechanism, and it is deliberately small:

```sh
tools/update-check.sh                          # check the configured channel
tools/update-check.sh --manifest URL_OR_PATH   # check a specific manifest
tools/update-check.sh --local                  # what is this tree, exactly? (no network)
```

A **manifest** is a JSON document listing a version, a channel, and every artifact with its SHA-256
and size. `tools/release.py` produces it from the tree:

```sh
tools/release.py manifest --version 0.4.0 --channel stable -o haki release.json
```

and verifies a tree against one:

```sh
tools/release.py verify release.json           # every artifact present and matching?
```

### Signing, and why it is in the tool rather than in a promise

A manifest that only carries hashes tells you a download was not corrupted *in transit*. It does not
tell you it is ours — anyone can serve a manifest with their own hashes. So `release.py` signs the
manifest with an OpenSSL key when one is available:

```sh
tools/release.py sign   release.json --key release.key   # -> release.json.sig
tools/release.py verify-signature release.json --sig release.json.sig --pub release.pub
```

`update-check.sh` **refuses to report an update from an unsigned manifest** unless you pass
`--allow-unsigned`, and says which it did. That is the honest default: the signature is the only
part of this that is not decorative.

There is no key in this repository. A private signing key in a public repo is a signed invitation to
forge releases, so `tools/release.py keygen` makes one locally and `.gitignore` refuses to track it.
Publishing a real channel means generating the key once, keeping the private half off the machine
that builds, and putting the public half in `release.pub` here.

## 4. Applying an update is a reboot, not a hot patch

Because HakiOS is the kernel, there is no "apply and continue":

* **RAM-only run (`fastboot boot`)** — an update is a new `boot.img` and a reboot. Nothing is
  written; a plain reboot returns the phone to Android unchanged.
* **Flashed boot partition** — an update replaces the boot partition and reboots. This is where you
  need a recovery path that actually exists, which is why `devices/redmi-14c/README.md` says nothing
  is flashed on the Redmi 14C yet.
* **Inside a running system** — user-space files *could* be replaced live, but the kernel and loader
  cannot. Promising fine-grained live updates for a kernel that is currently a RAM image would be
  false.

## 5. What this deliberately does not do

| Not done | Why |
|---|---|
| fetch or install Apple's firmware | it is Apple's, and the device will refuse it anyway |
| impersonate a device to Apple's TSS | it would be a lie, it would not work, and it is the one thing this tree's `COMPLIANCE.md` forbids |
| claim an update path for hardware nobody has reversed | the Redmi 14C's boot chain is not reverse engineered far enough to promise recovery from a bad flash |
| auto-update silently | an OS you boot from RAM should not replace itself without you asking |

## 6. Verifying all of it

```sh
tools/test/run-release-test.sh     # manifest, verify, tamper detection, version compare, signature
```

The test builds a manifest over a scratch tree, verifies it, then flips one byte in one artifact and
requires verification to *fail* — because a verifier that only ever says "ok" is not a verifier. It
also checks that an unsigned manifest is rejected by default and a signed one is accepted, so the
signature path is exercised rather than assumed.

Nothing in this document needs the phone, Apple's servers, or the network. That is on purpose: an
update mechanism you cannot test offline is one you cannot trust.
