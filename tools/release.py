#!/usr/bin/env python3
"""
HakiOS release manifests: make one, check one, sign one, verify one.

This is the machinery behind docs/updates.md. It exists because a HakiOS install has no way to ask
Apple for an update (only Apple can authorise an install of iOS onto an Apple device -- see that
document), so it needs a channel for its *own* files instead. A channel has to answer three things:

  what is the release?    a manifest: version, channel, and every artifact's SHA-256 and size
  is this copy intact?    verify: hash the tree, compare against the manifest
  is this manifest ours?  sign / verify-signature, with OpenSSL

The third is the one hashes cannot answer, because anyone can serve a manifest containing their own
hashes. So an unsigned manifest is not trusted by default (tools/update-check.sh refuses one unless
told --allow-unsigned), and no private key is kept in this repository -- `keygen` makes one locally
and .gitignore refuses to track it.

usage:
  tools/release.py manifest --version 0.4.0 [--channel stable] [-o release.json] [--root DIR]
  tools/release.py verify release.json [--root DIR] [--allow-unsigned]
  tools/release.py sign release.json --key release.key [-o release.json.sig]
  tools/release.py verify-signature release.json --sig release.json.sig --pub release.pub
  tools/release.py keygen [--key release.key] [--pub release.pub]
  tools/release.py is-newer A B        # exit 0 if A is newer than B, 1 if not
  tools/release.py show release.json
"""
import argparse
import fnmatch
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

SCHEMA = 1
NAME = "HakiOS"

# What ships. Globs rather than a fixed list, but a build that produced nothing is an error rather
# than a short manifest that silently verifies against itself.
ARTIFACTS = [
    "prebuilt/mach.development.bcm2837",
    "prebuilt/loader.efi",
    "prebuilt/userland/*",
    "xnuloader/loader.efi",
    "mtkshim/shim.bin",
    "devices/poco-x5-pro/prebuilt/*",
    "devices/redmi-14c/boot.img",
]
# Never hashed, even when a glob above would catch it: build output, keys, manifests, checksums.
EXCLUDE = ["*.o", "*.elf", "*.sig", "*.key", "release.json", "SHA256SUMS", "*.dtb", "*.args"]


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def excluded(rel):
    base = os.path.basename(rel)
    return any(fnmatch.fnmatch(base, pat) for pat in EXCLUDE)


def collect(root):
    """Every artifact that exists under root, as a sorted list of {path, sha256, size}."""
    found, out = {}, []
    for pattern in ARTIFACTS:
        for path in sorted(glob.glob(os.path.join(root, pattern))):
            if not os.path.isfile(path):
                continue
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if rel in found or excluded(rel):
                continue
            found[rel] = True
            out.append({"path": rel, "sha256": sha256_file(path), "size": os.path.getsize(path)})
    out.sort(key=lambda a: a["path"])
    return out


def load(path):
    with open(path) as f:
        return json.load(f)


def dump(man, path):
    text = json.dumps(man, indent=2, sort_keys=True) + "\n"
    if path == "-":
        sys.stdout.write(text)
    else:
        with open(path, "w") as f:
            f.write(text)


def version_tuple(v):
    """'0.4.0' -> (0,4,0). Anything non-numeric sorts before a number, so 0.4.0-rc1 < 0.4.0."""
    parts = []
    for chunk in str(v).replace("-", ".").split("."):
        try:
            parts.append((0, int(chunk)))
        except ValueError:
            parts.append((1, chunk))
    return parts


def newer(a, b):
    pa, pb = version_tuple(a), version_tuple(b)
    for i in range(max(len(pa), len(pb))):
        x = pa[i] if i < len(pa) else (0, 0)
        y = pb[i] if i < len(pb) else (0, 0)
        if x != y:
            return x > y
    return False


# ------------------------------------------------------------------ commands

def cmd_manifest(a):
    root = a.root or repo_root()
    arts = collect(root)
    if not arts:
        sys.stderr.write("release.py: no artifacts under %s -- nothing to release\n" % root)
        return 1
    man = {
        "schema": SCHEMA,
        "name": NAME,
        "version": a.version,
        "channel": a.channel,
        "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "artifacts": arts,
    }
    dump(man, a.output)
    if a.output != "-":
        sys.stderr.write("release.py: %d artifacts, %d bytes, version %s -> %s\n"
                         % (len(arts), sum(x["size"] for x in arts), a.version, a.output))
    return 0


def verify_against(man, root, problems):
    """Compare a manifest to a tree. Appends human-readable problems; returns how many matched."""
    if man.get("schema") != SCHEMA:
        problems.append("manifest schema %r, expected %d" % (man.get("schema"), SCHEMA))
    arts = man.get("artifacts") or []
    if not arts:
        problems.append("manifest lists no artifacts")
        return 0
    good = 0
    for entry in arts:
        rel = art_path(entry) if isinstance(entry, dict) else str(entry)
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            problems.append("missing: %s" % rel)
            continue
        if not isinstance(entry, dict):
            good += 1
            continue
        digest = sha256_file(path)
        if digest != entry.get("sha256"):
            problems.append("changed: %s (manifest %s..., file %s...)"
                            % (rel, str(entry.get("sha256"))[:12], digest[:12]))
            continue
        good += 1
    return good


def art_path(entry):
    return entry.get("path") or ""


def cmd_verify(a):
    man = load(a.manifest)
    root = a.root or repo_root()
    problems = []
    good = verify_against(man, root, problems)
    if problems:
        for p in problems:
            print("release.py: %s" % p, file=sys.stderr)
        print("release.py: verify FAILED (%d of %d artifacts ok)"
              % (good, len(man.get("artifacts") or [])), file=sys.stderr)
        return 1
    print("release.py: verify ok: %s %s (%s), %d artifacts"
          % (man.get("name", NAME), man.get("version", "?"), man.get("channel", "?"), good))
    return 0


def openssl():
    exe = shutil.which("openssl")
    if not exe:
        sys.stderr.write("release.py: no openssl on PATH (apt install openssl)\n")
    return exe


def cmd_keygen(a):
    exe = openssl()
    if not exe:
        return 1
    if os.path.exists(a.key):
        sys.stderr.write("release.py: %s already exists; refusing to overwrite a signing key\n" % a.key)
        return 1
    # A 2048-bit RSA key: universally available, and ed25519 is not accepted by every openssl build.
    rc = subprocess.call([exe, "genrsa", "-out", a.key, "2048"], stderr=subprocess.DEVNULL)
    if rc != 0:
        sys.stderr.write("release.py: openssl genrsa failed\n")
        return 1
    os.chmod(a.key, 0o600)
    rc = subprocess.call([exe, "rsa", "-in", a.key, "-pubout", "-out", a.pub],
                         stderr=subprocess.DEVNULL)
    if rc != 0:
        sys.stderr.write("release.py: openssl rsa -pubout failed\n")
        return 1
    print("release.py: wrote %s (private, keep it off the build machine) and %s (publishable)"
          % (a.key, a.pub))
    return 0


def cmd_sign(a):
    exe = openssl()
    if not exe:
        return 1
    out = a.output or (a.manifest + ".sig")
    # Sign the file's bytes: the signature covers the manifest exactly as shipped, so re-formatting
    # the JSON invalidates it, which is the behaviour we want.
    rc = subprocess.call([exe, "dgst", "-sha256", "-sign", a.key, "-out", out, a.manifest])
    if rc != 0:
        sys.stderr.write("release.py: openssl dgst -sign failed\n")
        return 1
    print("release.py: signed %s -> %s" % (a.manifest, out))
    return 0


def cmd_verify_signature(a):
    exe = openssl()
    if not exe:
        return 1
    for need in (a.manifest, a.sig, a.pub):
        if not os.path.isfile(need):
            sys.stderr.write("release.py: no %s\n" % need)
            return 1
    rc = subprocess.call([exe, "dgst", "-sha256", "-verify", a.pub, "-signature", a.sig, a.manifest],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if rc != 0:
        sys.stderr.write("release.py: signature does NOT verify against %s\n" % a.pub)
        return 1
    print("release.py: signature verifies against %s" % a.pub)
    return 0


def cmd_is_newer(a):
    return 0 if newer(a.a, a.b) else 1


def cmd_show(a):
    man = load(a.manifest)
    print("%s %s (%s), schema %d, %d artifacts, created %s"
          % (man.get("name", "?"), man.get("version", "?"), man.get("channel", "?"),
             man.get("schema", 0), len(man.get("artifacts") or []), man.get("created", "?")))
    for entry in man.get("artifacts") or []:
        if isinstance(entry, dict):
            print("  %12d  %s  %s" % (entry.get("size", 0), str(entry.get("sha256"))[:16],
                                      art_path(entry)))
        else:
            print("  %12s  %s" % ("", entry))
    return 0


def main():
    p = argparse.ArgumentParser(prog="release.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("manifest", help="write a manifest for this tree")
    m.add_argument("--version", required=True)
    m.add_argument("--channel", default="stable")
    m.add_argument("-o", "--output", default="release.json")
    m.add_argument("--root")
    m.set_defaults(fn=cmd_manifest)

    v = sub.add_parser("verify", help="check a tree against a manifest")
    v.add_argument("manifest")
    v.add_argument("--root")
    v.add_argument("--allow-unsigned", action="store_true",
                   help="accepted for symmetry with update-check.sh; verify never checks signatures")
    v.set_defaults(fn=cmd_verify)

    s = sub.add_parser("sign", help="sign a manifest")
    s.add_argument("manifest")
    s.add_argument("--key", default="release.key")
    s.add_argument("-o", "--output")
    s.set_defaults(fn=cmd_sign)

    vs = sub.add_parser("verify-signature", help="verify a manifest's signature")
    vs.add_argument("manifest")
    vs.add_argument("--sig", default=None)
    vs.add_argument("--pub", default="release.pub")
    vs.set_defaults(fn=cmd_verify_signature)

    k = sub.add_parser("keygen", help="make a local signing key")
    k.add_argument("--key", default="release.key")
    k.add_argument("--pub", default="release.pub")
    k.set_defaults(fn=cmd_keygen)

    n = sub.add_parser("is-newer", help="exit 0 if A is a newer version than B")
    n.add_argument("a")
    n.add_argument("b")
    n.set_defaults(fn=cmd_is_newer)

    sh = sub.add_parser("show", help="print a manifest")
    sh.add_argument("manifest")
    sh.set_defaults(fn=cmd_show)

    a = p.parse_args()
    if getattr(a, "cmd", None) == "verify-signature" and not a.sig:
        a.sig = a.manifest + ".sig"
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
