#!/bin/sh
# Installs xrizer (OpenVR on OpenXR, https://github.com/Supreeeme/xrizer) as the OpenVR runtime the streamer uses for
# OpenVR games: ${XDG_DATA_HOME:-~/.local/share}/opendesk/xrizer-build/bin/linux64/vrclient.so (+ version.txt)
#   get_xrizer.sh [nightly]         xrizer's own CI build of main (default; no toolchain needed)
#   get_xrizer.sh source [COMMIT]   build from the official source at COMMIT (git + Rust; `cargo --locked`)
# The new runtime replaces the old one only once it is complete and checked (64-bit ELF exporting VRClientCoreFactory).
set -eu
MODE=${1:-nightly}
NIGHTLY=https://nightly.link/Supreeeme/xrizer/workflows/ci/main/xrizer-nightly-release.zip
REPO=https://github.com/Supreeeme/xrizer.git
DATA=${XDG_DATA_HOME:-$HOME/.local/share}/opendesk
OUT=$DATA/xrizer-build
mkdir -p "$DATA"
STAGE=$(mktemp -d "$DATA/xrizer-stage.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/out/bin/linux64"

check() { # a usable OpenVR client runtime?
	[ "$(head -c 5 "$1" | od -An -c | tr -d ' \n')" = "177ELF002" ] || { echo "not a 64-bit ELF: $1" >&2; exit 1; }
	grep -q VRClientCoreFactory "$1" || { echo "no VRClientCoreFactory in $1" >&2; exit 1; }
}

case "$MODE" in
nightly)
	command -v curl >/dev/null && command -v unzip >/dev/null || { echo "needs curl and unzip" >&2; exit 1; }
	echo "downloading xrizer nightly ($NIGHTLY)"
	curl -fsSL --show-error --retry 2 -o "$STAGE/xrizer.zip" "$NIGHTLY"
	unzip -q "$STAGE/xrizer.zip" -d "$STAGE/zip"
	LIB=$(find "$STAGE/zip" -path "*/bin/linux64/vrclient.so" | head -1)
	[ -n "$LIB" ] || { echo "the nightly archive has no bin/linux64/vrclient.so" >&2; exit 1; }
	check "$LIB"
	cp "$LIB" "$STAGE/out/bin/linux64/vrclient.so"
	echo "nightly $(date -u +%Y-%m-%d) sha256:$(sha256sum "$LIB" | cut -c1-16)" > "$STAGE/out/version.txt"
	;;
source)
	COMMIT=${2:-0989a7fac2d1efb7ea82f5fe1a8ed30c3eeb9596}
	for tool in git cargo; do
		command -v $tool >/dev/null 2>&1 || { echo "building xrizer needs $tool (git and a Rust toolchain)" >&2; exit 1; }
	done
	SRC=$DATA/xrizer-src
	if [ ! -d "$SRC/.git" ]; then
		rm -rf "$SRC"
		echo "fetching xrizer source from $REPO"
		git clone --quiet --filter=blob:none "$REPO" "$SRC"
	fi
	git -C "$SRC" fetch --quiet origin "$COMMIT" 2>/dev/null || git -C "$SRC" fetch --quiet origin
	git -C "$SRC" checkout --quiet --force --detach "$COMMIT"
	git -C "$SRC" submodule --quiet update --init --recursive
	[ "$(git -C "$SRC" rev-parse HEAD)" = "$COMMIT" ] || { echo "checkout of $COMMIT failed" >&2; exit 1; }
	echo "building xrizer $COMMIT (release)"
	(cd "$SRC" && XRIZER_VERSION="opendesk-$(echo "$COMMIT" | cut -c1-12)" cargo xbuild --release --locked)
	LIB="$SRC/target/release/bin/linux64/vrclient.so"
	check "$LIB"
	cp -L "$LIB" "$STAGE/out/bin/linux64/vrclient.so"
	echo "source $COMMIT" > "$STAGE/out/version.txt"
	;;
*)
	echo "usage: $0 [nightly | source [COMMIT]]" >&2
	exit 2
	;;
esac

rm -rf "$OUT.old"
[ -d "$OUT" ] && mv "$OUT" "$OUT.old"
mv "$STAGE/out" "$OUT"
rm -rf "$OUT.old"
echo "xrizer installed: $OUT ($(cat "$OUT/version.txt"))"
