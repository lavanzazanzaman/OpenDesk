#!/usr/bin/env bash
# Builds the release files on this machine; nothing is built on GitHub.
#
#   tools/release.sh [--publish]
#
# The version comes from project(... VERSION ...) in CMakeLists.txt. dist/ receives:
#   OpenDesk-Linux-x86_64.AppImage         portable, updates itself from the latest GitHub release
#   OpenDesk-Linux-x86_64-native.tar.gz    built against this machine's libraries
#   SHA256SUMS                             read by the AppImage's updater
# --publish then tags the commit (v<version>) and creates the GitHub release from those files.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
PUBLISH=0
case "${1:-}" in
"") ;;
--publish) PUBLISH=1 ;;
*) sed -n '2,10p' "$0"; exit 2 ;;
esac

VERSION=$(sed -nE 's/^project\(opendesk VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)
[ -n "$VERSION" ] || { echo "no version in CMakeLists.txt" >&2; exit 1; }
TAG=v$VERSION
if [ $PUBLISH = 1 ]; then
	[ -z "$(git status --porcelain)" ] || { echo "commit your changes first" >&2; exit 1; }
	if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
		echo "$TAG already exists: bump the version in CMakeLists.txt" >&2
		exit 1
	fi
fi

echo "== AppImage ($VERSION)"
bash tools/build-appimage.sh

echo "== native binary"
NATIVE=build-release
cmake -S . -B "$NATIVE" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/OpenDesk -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$NATIVE" -j"$(nproc)"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
DESTDIR="$STAGE" cmake --install "$NATIVE" >/dev/null
cp README.md LICENSE THIRD_PARTY_NOTICES.md "$STAGE/OpenDesk/"
tar -C "$STAGE" -czf dist/OpenDesk-Linux-x86_64-native.tar.gz OpenDesk

(cd dist && sha256sum OpenDesk-Linux-x86_64.AppImage OpenDesk-Linux-x86_64-native.tar.gz > SHA256SUMS && cat SHA256SUMS)

if [ $PUBLISH = 0 ]; then
	echo "Built $TAG in dist/. Run with --publish to release it."
	exit 0
fi
git tag "$TAG"
git push origin HEAD "$TAG"
gh release create "$TAG" --title "OpenDesk $VERSION" --generate-notes \
	dist/OpenDesk-Linux-x86_64.AppImage dist/OpenDesk-Linux-x86_64-native.tar.gz dist/SHA256SUMS
