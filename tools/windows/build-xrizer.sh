#!/usr/bin/env bash
# Builds OpenDesk's xrizer fork (GPL-3.0, a separate repository: OpenVR on OpenXR, with a Direct3D 11 backend and WGL)
# for Windows and stages the runtime the streamer registers for OpenVR games:
#   <out>/bin/win64/vrclient_x64.dll + openxr_loader.dll (+ the loader's C++ runtime) + version.txt
#
#   tools/windows/build-xrizer.sh [XRIZER_SOURCE] [OUT]
# XRIZER_SOURCE defaults to ../xrizer-opendesk, OUT to build-xrizer-win. Needs rustup's x86_64-pc-windows-gnullvm
# target, llvm-mingw (LLVM_MINGW) and the Windows sysroot of tools/windows/fetch-sysroot.py (OPENDESK_WIN_SYSROOT).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${1:-$(cd "$ROOT/.." && pwd)/xrizer-opendesk}
OUT=${2:-$ROOT/build-xrizer-win}
LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
SYSROOT=${OPENDESK_WIN_SYSROOT:-$(cd "$ROOT/.." && pwd)/toolchain/win-sysroot/clang64}
TARGET=x86_64-pc-windows-gnullvm

[ -f "$SRC/Cargo.toml" ] || { echo "no xrizer source at $SRC" >&2; exit 1; }
rustup target list --installed | grep -qx $TARGET || rustup target add $TARGET
COMMIT=$(git -C "$SRC" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
(cd "$SRC" && XRIZER_VERSION="opendesk-$COMMIT" cargo build --release --target $TARGET --no-default-features)

rm -rf "$OUT"
mkdir -p "$OUT/bin/win64"
cp "$SRC/target/$TARGET/release/xrizer.dll" "$OUT/bin/win64/vrclient_x64.dll"
cp "$SYSROOT/bin/libopenxr_loader.dll" "$OUT/bin/win64/openxr_loader.dll"
cp "$LLVM_MINGW/x86_64-w64-mingw32/bin/libc++.dll" "$LLVM_MINGW/x86_64-w64-mingw32/bin/libunwind.dll" "$OUT/bin/win64/"
echo "source $COMMIT (Windows)" > "$OUT/version.txt"
grep -q VRClientCoreFactory "$OUT/bin/win64/vrclient_x64.dll" || { echo "vrclient_x64.dll exports no VRClientCoreFactory" >&2; exit 1; }
echo "xrizer for Windows: $OUT"
