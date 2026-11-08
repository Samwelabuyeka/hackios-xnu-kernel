#!/bin/bash
# Test the release/update machinery: tools/release.py and tools/update-check.sh.
#
# The point of these tools is to say "no" correctly, so most of this test is about the refusals:
# a verifier that only ever says ok is not a verifier, and an update channel that accepts any
# manifest is a way to get someone to boot a stranger's kernel.
#
#   tools/test/run-release-test.sh
#
# Needs python3 and openssl. No network, no phone.
#
# Deliberately NOT `set -e`: half of these assertions require a command to fail, and errexit would
# abort the script at the first expected failure instead of counting it.
set +e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
cd "$REPO"

REL=tools/release.py
CHK=tools/update-check.sh
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

fail=0
check() {  # check <description> <expected-rc> <actual-rc>
	if [ "$2" = "$3" ]; then
		printf '  ok   %s\n' "$1"
	else
		printf '  FAIL %s (expected rc=%s, got rc=%s)\n' "$1" "$2" "$3"
		fail=$((fail + 1))
	fi
}

# a tree shaped like this one, small enough to hash instantly
mkdir -p "$T/tree/prebuilt/userland" "$T/tree/xnuloader" "$T/tree/mtkshim"
printf 'kernel' > "$T/tree/prebuilt/mach.development.bcm2837"
printf 'loader' > "$T/tree/prebuilt/loader.efi"
printf 'hello'  > "$T/tree/prebuilt/userland/hello"
printf 'init'   > "$T/tree/prebuilt/userland/init"
printf 'shim'   > "$T/tree/mtkshim/shim.bin"

echo "manifest"
python3 "$REL" manifest --version 1.2.3 --root "$T/tree" -o "$T/m.json" 2>/dev/null
check "a manifest is produced" 0 $?
n=$(python3 -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["artifacts"]))' "$T/m.json")
check "it lists all five artifacts (got $n)" 5 "$n"
v=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["version"])' "$T/m.json")
check "it carries the version (got $v)" "1.2.3" "$v"
# build output and keys must never be hashed, or a rebuild silently invalidates a release
printf 'x' > "$T/tree/mtkshim/shim.elf"
printf 'x' > "$T/tree/prebuilt/userland/.DS_Store"
python3 "$REL" manifest --version 1.2.3 --root "$T/tree" -o "$T/m2.json" 2>/dev/null
n2=$(python3 -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["artifacts"]))' "$T/m2.json")
check "excluded files (.elf, .DS_Store) are not hashed" "$n" "$n2"

echo "verify"
python3 "$REL" verify "$T/m.json" --root "$T/tree" >/dev/null 2>&1
check "an intact tree verifies" 0 $?

printf 'hello!' > "$T/tree/prebuilt/userland/hello"       # one byte more
python3 "$REL" verify "$T/m.json" --root "$T/tree" >/dev/null 2>&1
check "one changed byte is caught" 1 $?

printf 'hello' > "$T/tree/prebuilt/userland/hello"        # put it back
rm "$T/tree/prebuilt/userland/init"
python3 "$REL" verify "$T/m.json" --root "$T/tree" >/dev/null 2>&1
check "a missing artifact is caught" 1 $?

printf 'init' > "$T/tree/prebuilt/userland/init"
python3 "$REL" verify "$T/m.json" --root "$T/tree" >/dev/null 2>&1
check "restoring it makes the tree verify again" 0 $?

# a manifest that describes nothing must not pass
printf '{"schema":1,"name":"HakiOS","version":"1.0.0","artifacts":[]}\n' > "$T/empty.json"
python3 "$REL" verify "$T/empty.json" --root "$T/tree" >/dev/null 2>&1
check "an empty manifest is refused" 1 $?

# wrong schema must not pass
printf '{"schema":99,"version":"1.0.0","artifacts":[{"path":"prebuilt/loader.efi","sha256":"00"}]}\n' > "$T/schema.json"
python3 "$REL" verify "$T/schema.json" --root "$T/tree" >/dev/null 2>&1
check "an unknown schema is refused" 1 $?

echo "versions"
python3 "$REL" is-newer 1.2.4 1.2.3; check "1.2.4 is newer than 1.2.3" 0 $?
python3 "$REL" is-newer 1.2.3 1.2.4; check "1.2.3 is not newer than 1.2.4" 1 $?
python3 "$REL" is-newer 1.2.3 1.2.3; check "a version is not newer than itself" 1 $?
python3 "$REL" is-newer 1.10.0 1.9.9; check "1.10.0 sorts above 1.9.9, not below" 0 $?
python3 "$REL" is-newer 2.0.0 1.99.99; check "2.0.0 is newer than 1.99.99" 0 $?

echo "signature"
if command -v openssl >/dev/null; then
	python3 "$REL" keygen --key "$T/k.pem" --pub "$T/p.pem" >/dev/null 2>&1
	check "keygen makes a key pair" 0 $?
	# a second keygen must refuse to overwrite a signing key
	python3 "$REL" keygen --key "$T/k.pem" --pub "$T/p.pem" >/dev/null 2>&1
	check "keygen refuses to overwrite an existing key" 1 $?

	python3 "$REL" sign "$T/m.json" --key "$T/k.pem" >/dev/null 2>&1
	check "sign produces a signature" 0 $?
	python3 "$REL" verify-signature "$T/m.json" --sig "$T/m.json.sig" --pub "$T/p.pem" >/dev/null 2>&1
	check "the signature verifies" 0 $?

	# the whole point: a changed manifest must not verify
	sed 's/1\.2\.3/9.9.9/' "$T/m.json" > "$T/tampered.json"
	python3 "$REL" verify-signature "$T/tampered.json" --sig "$T/m.json.sig" --pub "$T/p.pem" >/dev/null 2>&1
	check "a tampered manifest fails its signature" 1 $?

	# and a signature must not verify against someone else's key
	python3 "$REL" keygen --key "$T/other.key" --pub "$T/other.pub" >/dev/null 2>&1
	python3 "$REL" verify-signature "$T/m.json" --sig "$T/m.json.sig" --pub "$T/other.pub" >/dev/null 2>&1
	check "a signature does not verify against a different key" 1 $?

	# a manifest with no signature file at all is refused
	rm -f "$T/m.json.sig"
	python3 "$REL" verify-signature "$T/m.json" --sig "$T/m.json.sig" --pub "$T/p.pem" >/dev/null 2>&1
	check "a missing signature file is refused" 1 $?
	python3 "$REL" sign "$T/m.json" --key "$T/k.pem" >/dev/null 2>&1
else
	echo "  skip signature tests (no openssl)"
fi

echo "channel checker"
# an unsigned manifest is refused by default
env -u HAKIOS_RELEASE_PUB "$CHK" --manifest "$T/unsigned.json" >/dev/null 2>&1 || true
python3 -c 'import json;d=json.load(open("'$T'/m.json"));print(json.dumps(d))' > "$T/unsigned.json"
env -u HAKIOS_RELEASE_PUB "$CHK" --manifest "$T/unsigned.json" >/dev/null 2>&1
check "an unsigned manifest is refused without --allow-unsigned" 1 $?
env -u HAKIOS_RELEASE_PUB "$CHK" --manifest "$T/unsigned.json" --allow-unsigned --version 9.9.9 >/dev/null 2>&1
check "the same manifest is accepted with --allow-unsigned" 0 $?
env -u HAKIOS_RELEASE_PUB "$CHK" --manifest "$T/unsigned.json" --allow-unsigned --version 1.0.0 >/dev/null 2>&1
check "a newer release in the channel exits 10" 10 $?
env -u HAKIOS_RELEASE_PUB "$CHK" --manifest "$T/unsigned.json" --allow-unsigned --version 1.2.3 >/dev/null 2>&1
check "the same version in the channel is up to date (0)" 0 $?

# a manifest that is not JSON must be refused, not downloaded over
printf 'this is not json\n' > "$T/junk.json"
env -u HAKIOS_RELEASE_PUB "$CHK" --manifest "$T/junk.json" --allow-unsigned >/dev/null 2>&1
check "a manifest that is not JSON is refused" 1 $?

# a channel that does not exist is not an error in the installation
"$CHK" --manifest "$T/no-such-channel.json" >/dev/null 2>&1
check "an unreachable channel exits 0 (nothing published, not a fault)" 0 $?

echo
if [ "$fail" -ne 0 ]; then
	echo "run-release-test.sh: $fail failure(s)"
	exit 1
fi
echo "run-release-test.sh: ok"
echo "  These are the refusals that matter: a changed artifact, a missing artifact, an empty or"
echo "  unknown-schema manifest, an unsigned manifest, a bad signature. Apple's updates cannot be"
echo "  fetched this way and nothing here pretends otherwise -- see docs/updates.md."
