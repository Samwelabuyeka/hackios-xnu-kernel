#!/bin/bash
# Check the HakiOS update channel: is there a newer release than this tree, and is its manifest one
# we trust?
#
# This is the user-facing half of docs/updates.md. The question it answers is NOT "is there a new
# iOS" -- only Apple can authorise an install of iOS onto an Apple device, so no tool here can ask
# for one. It answers "has a newer set of HakiOS's OWN files been published, and is that manifest
# intact and signed?"
#
# Trust: a manifest carrying only hashes proves a download was not corrupted, not that it is ours --
# anyone can publish a manifest with their own hashes in it. So an unsigned manifest is refused
# unless you pass --allow-unsigned, and the tool says plainly which case happened.
#
#   tools/update-check.sh [--manifest URL_OR_PATH] [--version V] [--allow-unsigned] [--local]
#
#   --local     do not touch the network: report what this tree is (version, artifacts, checksums)
#   --manifest  where the channel's manifest is (default $HAKIOS_MANIFEST, else the URL below)
#
# Exit: 0 = up to date, 10 = a newer release is available, 1 = error (bad manifest, failed
# signature, unreadable channel). 10 rather than 0 so a script can tell "newer" from "current"
# without parsing output.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
cd "$REPO"

REL="$HERE/release.py"
MANIFEST=${HAKIOS_MANIFEST:-https://github.com/Samwelabuyeka/hackios-xnu-kernel/releases/latest/download/release.json}
PUB=${HAKIOS_RELEASE_PUB:-release.pub}
WANT_VERSION=""
ALLOW_UNSIGNED=0
LOCAL=0

while [ $# -gt 0 ]; do
	case $1 in
	--manifest) MANIFEST=$2; shift 2 ;;
	--version) WANT_VERSION=$2; shift 2 ;;
	--allow-unsigned) ALLOW_UNSIGNED=1; shift ;;
	--local) LOCAL=1; shift ;;
	-h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "update-check.sh: unknown argument $1" >&2; exit 1 ;;
	esac
done

command -v python3 >/dev/null || { echo "update-check.sh: no python3" >&2; exit 1; }

# ------------------------------------------------------------------ --local: what is this tree?

if [ "$LOCAL" = 1 ]; then
	TMP=$(mktemp -d)
	trap 'rm -rf "$TMP"' EXIT
	if ! python3 "$REL" manifest --version "$(git describe --tags --always 2>/dev/null || echo unknown)" \
		-o "$TMP/local.json"; then
		echo "update-check.sh: could not describe this tree" >&2
		exit 1
	fi
	echo "This tree:"
	python3 "$REL" show "$TMP/local.json" | sed 's/^/  /'
	echo
	echo "  commit: $(git rev-parse --short HEAD 2>/dev/null || echo '(not a git checkout)')"
	echo "  branch: $(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')"
	exit 0
fi

# ------------------------------------------------------------------ fetch the channel's manifest

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
LOCAL_MANIFEST="$TMP/release.json"
LOCAL_SIG="$TMP/release.json.sig"

fetch() {   # fetch <src> <dst>; a plain path is copied, anything else is downloaded
	case $1 in
	/*|./*|../*) cp "$1" "$2" ;;
	*) command -v curl >/dev/null || { echo "update-check.sh: no curl for $1" >&2; return 1; }
	   curl -fsSL --max-time 30 -o "$2" "$1" ;;
	esac
}

echo "channel: $MANIFEST"
if ! fetch "$MANIFEST" "$LOCAL_MANIFEST"; then
	echo "update-check.sh: cannot read the manifest (no network, or the channel has no releases yet)" >&2
	echo "update-check.sh: this is not an error in your installation -- it means nothing is published" >&2
	exit 0
fi

python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$LOCAL_MANIFEST" 2>/dev/null || {
	echo "update-check.sh: the manifest is not valid JSON -- refusing it" >&2; exit 1; }

# ------------------------------------------------------------------ is it signed, and by whom?

SIGNED=0
if fetch "$MANIFEST.sig" "$LOCAL_SIG" 2>/dev/null && [ -s "$LOCAL_SIG" ]; then
	if [ -f "$PUB" ]; then
		if python3 "$REL" verify-signature "$LOCAL_MANIFEST" --sig "$LOCAL_SIG" --pub "$PUB" >/dev/null 2>&1; then
			SIGNED=1
			echo "signature: verified against $PUB"
		else
			echo "update-check.sh: the manifest's signature does NOT verify against $PUB" >&2
			echo "update-check.sh: refusing an update from a manifest we cannot authenticate" >&2
			exit 1
		fi
	else
		echo "update-check.sh: the manifest is signed but $PUB is not here to check it against" >&2
		echo "update-check.sh: put the channel's public key at $PUB, or pass HAKIOS_RELEASE_PUB=..." >&2
		exit 1
	fi
fi

if [ "$SIGNED" = 0 ]; then
	if [ "$ALLOW_UNSIGNED" = 1 ]; then
		echo "signature: NONE -- accepted only because --allow-unsigned was given"
		echo "           hashes alone prove the download arrived intact, not that we published it"
	else
		echo "update-check.sh: refusing an unsigned manifest" >&2
		echo "update-check.sh: anyone can publish a manifest with their own hashes in it. Pass" >&2
		echo "                 --allow-unsigned if you have decided to trust this channel by other means." >&2
		exit 1
	fi
fi

# ------------------------------------------------------------------ compare versions

CHANNEL_VERSION=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("version") or "")' "$LOCAL_MANIFEST")
[ -n "$CHANNEL_VERSION" ] || { echo "update-check.sh: the manifest has no version" >&2; exit 1; }

HAVE=$WANT_VERSION
if [ -z "$HAVE" ]; then
	# the version this tree would describe itself as; a git tag if there is one
	HAVE=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')
	[ -n "$HAVE" ] || HAVE="0.0.0"
fi

echo "have:    $HAVE"
echo "channel: $CHANNEL_VERSION"

if python3 "$REL" is-newer "$CHANNEL_VERSION" "$HAVE"; then
	echo
	echo "A newer HakiOS ($CHANNEL_VERSION) is available."
	echo "  Manifest: $MANIFEST"
	echo "  Applying it is a reboot, not a hot patch: see docs/updates.md section 4."
	exit 10
fi
echo
echo "Up to date."
exit 0
