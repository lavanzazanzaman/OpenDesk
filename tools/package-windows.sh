#!/usr/bin/env bash
# Stage the Windows build for the NSIS installer. Run inside an MSYS2 MINGW64 shell after building:
#   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win
#   tools/package-windows.sh <build-dir>
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build-win}
DEST=${OPENDESK_STAGE_DIR:-$ROOT/dist/OpenDesk}
PREFIX=${MINGW_PREFIX:-/mingw64}

rm -rf "$DEST"
mkdir -p "$DEST"

EXE="$BUILD/streamer/opendesk-streamer.exe"
[ -f "$EXE" ] || { echo "missing $EXE" >&2; exit 1; }
cp "$EXE" "$DEST/OpenDesk.exe"

# Qt: platform plugins, image formats and the C++ runtime next to the executable.
DEPLOY_QT=$(command -v windeployqt6 || command -v windeployqt)
[ -n "$DEPLOY_QT" ] || { echo "windeployqt not found (install Qt 6 tools)" >&2; exit 1; }
"$DEPLOY_QT" --release --no-translations --no-system-d3d-compiler --no-opengl-sw \
    --no-quick-import --skip-plugin-types qmltooling "$DEST/OpenDesk.exe"

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
        if [ -n "$src" ] && [ -f "$src" ]; then
            cp "$src" "$DEST/$name"
            echo "  $name"
        fi
    done < <(objdump -p "$file" 2>/dev/null | awk '/DLL Name:/ {print $3}')
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

# FFmpeg's encoders load no external DLLs, but the runtime side needs its data files when present.
for d in "$PREFIX"/share/ffmpeg "$PREFIX"/share/opendesk; do
    [ -d "$d" ] && cp -r "$d" "$DEST/" || true
done

cp "$ROOT/LICENSE" "$DEST/LICENSE.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$DEST/THIRD_PARTY_NOTICES.txt"
printf 'OpenDesk\r\nVersion %s\r\n' "${OPENDESK_VERSION:-1.0}" > "$DEST/VERSION.txt"

echo "staged $DEST"
