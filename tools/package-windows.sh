#!/usr/bin/env bash
# Stage the Windows build for the NSIS installer. Run inside an MSYS2 CLANG64 shell after building:
#   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win
#   tools/package-windows.sh <build-dir>
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build-win}
DEST=${OPENDESK_STAGE_DIR:-$ROOT/dist/OpenDesk}
PREFIX=${MINGW_PREFIX:-/clang64}

rm -rf "$DEST"
mkdir -p "$DEST"

EXE="$BUILD/streamer/opendesk-streamer.exe"
[ -f "$EXE" ] || { echo "missing $EXE" >&2; exit 1; }
cp "$EXE" "$DEST/OpenDesk.exe"

# Cross-build from Linux (tools/windows/llvm-mingw.cmake): OPENDESK_WIN_SYSROOT=<sysroot>/clang64. DLLs come from the
# sysroot and llvm-mingw, the Qt plugins are copied by hand (no windeployqt on Linux).
OBJDUMP=objdump
if [ -n "${OPENDESK_WIN_SYSROOT:-}" ]; then
    LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
    OBJDUMP=$LLVM_MINGW/bin/x86_64-w64-mingw32-objdump
    PREFIX=$OPENDESK_WIN_SYSROOT
    export PATH="$OPENDESK_WIN_SYSROOT/bin:$LLVM_MINGW/x86_64-w64-mingw32/bin:$PATH"
    for kind in platforms styles imageformats tls networkinformation; do
        [ -d "$PREFIX/share/qt6/plugins/$kind" ] && { mkdir -p "$DEST/$kind"; cp "$PREFIX/share/qt6/plugins/$kind/"*.dll "$DEST/$kind/"; }
    done
    printf '[Paths]\r\nPlugins = .\r\n' > "$DEST/qt.conf"
else
    # Qt: platform plugins, image formats and the C++ runtime next to the executable.
    DEPLOY_QT=$(command -v windeployqt6 || command -v windeployqt)
    [ -n "$DEPLOY_QT" ] || { echo "windeployqt not found (install Qt 6 tools)" >&2; exit 1; }
    "$DEPLOY_QT" --release --no-translations --no-system-d3d-compiler --no-opengl-sw \
        --no-quick-import --skip-plugin-types qmltooling "$DEST/OpenDesk.exe"
fi

# Copy every MinGW DLL the executable and its plugins pull in (FFmpeg, OpenSSL, lz4, ...) transitively.
# objdump is part of binutils (already installed); this avoids depending on ldd/ntldd behaviour.
copy_missing_deps() {
    local file=$1 name src
    while IFS= read -r name; do
        case "$name" in
            api-ms-win-*|ext-ms-*|KERNEL32.dll|KERNELBASE.dll|USER32.dll|GDI32.dll|ADVAPI32.dll|\
            SHELL32.dll|ole32.dll|OLEAUT32.dll|WS2_32.dll|WINMM.dll|IPHLPAPI.dll|D3D11.dll|DXGI.dll|\
            SETUPAPI.dll|SHLWAPI.dll|COMCTL32.dll|COMDLG32.dll|UxTheme.dll|dwmapi.dll|ntdll.dll|\
            RPCRT4.dll|CRYPT32.dll|BCRYPT.dll|secur32.dll|VERSION.dll|POWRPROF.dll|DNSAPI.dll|\
            bcryptprimitives.dll|USERENV.dll|MPR.dll|NETAPI32.dll|WTSAPI32.dll|D3DCOMPILER_47.dll)
                continue ;;
        esac
        [ -e "$DEST/$name" ] && continue
        src=$(command -v "$name" 2>/dev/null || true)
        if [ -z "$src" ]; then # DLLs without the executable bit: look through PATH by hand
            for dir in ${PATH//:/ }; do
                for cand in "$dir/$name" "$dir/$(echo "$name" | tr '[:upper:]' '[:lower:]')"; do
                    [ -f "$cand" ] && { src=$cand; break 2; }
                done
            done
        fi
        if [ -n "$src" ] && [ -f "$src" ]; then
            cp "$src" "$DEST/$name"
            echo "  $name"
        fi
    done < <("$OBJDUMP" -p "$file" 2>/dev/null | awk '/DLL Name:/ {print $3}')
}

for _ in 1 2 3 4 5 6; do
    changed=0
    while IFS= read -r f; do
        before=$(find "$DEST" -type f | wc -l)
        copy_missing_deps "$f"
        [ "$(find "$DEST" -type f | wc -l)" != "$before" ] && changed=1
    done < <(find "$DEST" -type f \( -name '*.exe' -o -name '*.dll' \))
    [ "$changed" = 0 ] && break
done

# VR: OpenDesk XR + libovr-opendesk (self-contained: no C++ runtime DLLs) and, when built, xrizer for OpenVR games
# (tools/windows/build-xrizer.sh; default build-xrizer-win)
mkdir -p "$DEST/runtime"
cp "$BUILD/runtime/opendesk-openxr.dll" "$BUILD/runtime/ovr-vd.dll" "$BUILD/runtime/opendesk_xr.json" "$DEST/runtime/"
XRIZER_WIN=${OPENDESK_XRIZER_WIN:-$ROOT/build-xrizer-win}
if [ -f "$XRIZER_WIN/bin/win64/vrclient_x64.dll" ]; then
    mkdir -p "$DEST/xrizer"
    cp -r "$XRIZER_WIN/bin" "$XRIZER_WIN/version.txt" "$DEST/xrizer/"
    echo "  xrizer: $(cat "$XRIZER_WIN/version.txt")"
else
    echo "  no xrizer build (tools/windows/build-xrizer.sh): OpenVR games will need SteamVR" >&2
fi

# DXVK for the per-game "DXVK" switch (zlib license), pinned and checked
DXVK_VERSION=3.1.1
DXVK_SHA256=40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608
DOWNLOADS=${OPENDESK_DOWNLOADS:-$ROOT/../toolchain/downloads}
mkdir -p "$DOWNLOADS"
DXVK_TAR=$DOWNLOADS/dxvk-$DXVK_VERSION.tar.gz
[ -s "$DXVK_TAR" ] || curl -fsSL -o "$DXVK_TAR" "https://github.com/doitsujin/dxvk/releases/download/v$DXVK_VERSION/dxvk-$DXVK_VERSION.tar.gz"
echo "$DXVK_SHA256  $DXVK_TAR" | sha256sum -c --quiet - || { echo "DXVK download checksum mismatch" >&2; exit 1; }
mkdir -p "$DEST/dxvk/x64"
tar -xzf "$DXVK_TAR" -C "$DEST/dxvk/x64" --strip-components=2 "dxvk-$DXVK_VERSION/x64/d3d11.dll" "dxvk-$DXVK_VERSION/x64/dxgi.dll"
DXVK_LICENSE=$DOWNLOADS/dxvk-$DXVK_VERSION-LICENSE
[ -s "$DXVK_LICENSE" ] || curl -fsSL -o "$DXVK_LICENSE" "https://raw.githubusercontent.com/doitsujin/dxvk/v$DXVK_VERSION/LICENSE"
cp "$DXVK_LICENSE" "$DEST/dxvk/LICENSE.txt"
echo "$DXVK_VERSION" > "$DEST/dxvk/VERSION.txt"

# FFmpeg's encoders load no external DLLs, but the runtime side needs its data files when present.
for d in "$PREFIX"/share/ffmpeg "$PREFIX"/share/opendesk; do
    [ -d "$d" ] && cp -r "$d" "$DEST/" || true
done

cp "$ROOT/LICENSE" "$DEST/LICENSE.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$DEST/THIRD_PARTY_NOTICES.txt"
printf 'OpenDesk\r\nVersion %s\r\n' "${OPENDESK_VERSION:-1.0}" > "$DEST/VERSION.txt"

echo "staged $DEST"
