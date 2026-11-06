#!/bin/bash
# Rebuild the portable app and the older-ABI runtime. Requires Docker, curl,
# Python 3, git, Docker. Third-party code is vendored under third_party/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OPENDESK_BUILD_DIR:-$ROOT/build-portable}
mkdir -p "$OUT/downloads" "$OUT/openxr-include/openxr" "$ROOT/dist"
OUT=$(realpath "$OUT")
fetch() { [ -s "$2" ] || curl -fL --retry 2 "$1" -o "$2"; }
verify() { printf '%s  %s\n' "$1" "$2" | sha256sum -c -; }
for header in openxr.h openxr_platform.h openxr_platform_defines.h openxr_loader_negotiation.h \
              openxr_reflection.h openxr_reflection_parent_structs.h openxr_reflection_structs.h; do
    fetch "https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/release-1.1.60/include/openxr/$header" "$OUT/openxr-include/openxr/$header"
done
fetch https://nightly.link/Supreeeme/xrizer/actions/artifacts/10935346318.zip "$OUT/downloads/xrizer.zip"
python3 - "$OUT" <<'PY'
import pathlib, sys, zipfile
p=pathlib.Path(sys.argv[1]); z=zipfile.ZipFile(p/'downloads/xrizer.zip')
n=next(n for n in z.namelist() if n.endswith('bin/linux64/vrclient.so'))
d=p/'xrizer/bin/linux64/vrclient.so'; d.parent.mkdir(parents=True,exist_ok=True);d.write_bytes(z.read(n))
(p/'xrizer/version.txt').write_text('693f5d3103cf249adf2b6d1eca90f5be52d4b443\n')
(p/'xrizer/SOURCE.txt').write_text('https://github.com/Supreeeme/xrizer/tree/693f5d3103cf249adf2b6d1eca90f5be52d4b443\nLicense: GPL-3.0; see LICENSE\n')
PY
verify 3c123fa9f015f4ea98de68f84ba29c1fa3baef1e6b2d2aca9d5ea91c4cec3bfc "$OUT/xrizer/bin/linux64/vrclient.so"
fetch https://raw.githubusercontent.com/Supreeeme/xrizer/693f5d3103cf249adf2b6d1eca90f5be52d4b443/LICENSE "$OUT/xrizer/LICENSE"
fetch https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage "$OUT/downloads/appimagetool.AppImage"
verify a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0 "$OUT/downloads/appimagetool.AppImage"
fetch https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 "$OUT/downloads/runtime-x86_64"
verify 156f4bdbde9c52d01814600013e0a273f0118dc2de98975f3c8c63427ec79074 "$OUT/downloads/runtime-x86_64"
docker build -t opendesk-build:ubuntu24 - < "$ROOT/tools/Dockerfile.portable"
docker build -t opendesk-runtime:debian12 - < "$ROOT/tools/Dockerfile.runtime"
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src:ro" -v "$OUT:/out" opendesk-runtime:debian12 \
    sh -c 'cmake -S /src -B /out/runtime-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENDESK_RUNTIME_ONLY=ON -DCMAKE_CXX_FLAGS=-I/out/openxr-include && cmake --build /out/runtime-build -j 6'
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src:ro" -v "$OUT:/out" opendesk-build:ubuntu24 \
    sh -c 'cmake -S /src -B /out/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-I/out/openxr-include && cmake --build /out/build -j 6 && python3 /src/tools/package-appdir.py'
chmod +x "$OUT/downloads/appimagetool.AppImage"
(cd "$OUT/downloads"; ./appimagetool.AppImage --appimage-extract >/dev/null)
ARCH=x86_64 "$OUT/downloads/squashfs-root/AppRun" --runtime-file "$OUT/downloads/runtime-x86_64" \
    "$OUT/AppDir" "$ROOT/dist/OpenDesk-Linux-x86_64.AppImage"
(cd "$ROOT/dist"; sha256sum OpenDesk-Linux-x86_64.AppImage > SHA256SUMS)
