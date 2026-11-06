#!/usr/bin/env bash
# Local CI: everything the GitHub workflows do, on this machine, plus the tests they can't run (GPU, Wine).
#
#   tools/ci.sh [linux] [windows] [package] [--no-gpu] [--jobs N]
#
# With no stage named, runs linux, windows and package.
#   linux   - configure + build (build-ci/linux), then with a GPU: VR server <-> libovr-opendesk Vulkan app on every
#             DRM GPU (DMA-BUF and, on NVIDIA, exportable Vulkan memory) and NVENC/VAAPI test encodes
#   windows - cross-build with llvm-mingw + the MSYS2 CLANG64 sysroot (fetched on first use into ../toolchain),
#             then under Wine (private prefix in build-ci): VR pipe server <-> client, OpenXR loader -> OpenDesk XR
#   package - stage dist/OpenDesk (the Windows bundle); with makensis, also the installer; the Linux AppImage when
#             tools/build-appimage.sh's dependencies are present
# Exit status: non-zero on the first failing step; a summary is printed at the end.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CI=$ROOT/build-ci
TOOLCHAIN=${OPENDESK_TOOLCHAIN_DIR:-$(cd "$ROOT/.." && pwd)/toolchain}
SYSROOT=$TOOLCHAIN/win-sysroot/clang64
LLVM_MINGW=${LLVM_MINGW:-/opt/llvm-mingw}
JOBS=$(nproc)
GPU=1
STAGES=()
while [ $# -gt 0 ]; do
	case "$1" in
	linux | windows | package) STAGES+=("$1") ;;
	--no-gpu) GPU=0 ;;
	--jobs) JOBS=$2; shift ;;
	-h | --help) sed -n '2,15p' "$0"; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
	shift
done
[ ${#STAGES[@]} -eq 0 ] && STAGES=(linux windows package)
mkdir -p "$CI"
LOG=$CI/logs
mkdir -p "$LOG"
RESULTS=()

step() { # step NAME COMMAND...: runs it, output in build-ci/logs/NAME.log
	local name=$1
	shift
	printf '%-44s' "$name"
	local start=$SECONDS
	if "$@" >"$LOG/$name.log" 2>&1; then
		echo "ok   ($((SECONDS - start)) s)"
		RESULTS+=("ok    $name")
	else
		echo "FAIL ($((SECONDS - start)) s) - see build-ci/logs/$name.log"
		tail -15 "$LOG/$name.log" | sed 's/^/    /'
		RESULTS+=("FAIL  $name")
		summary
		exit 1
	fi
}
skip() {
	printf '%-44s%s\n' "$1" "skipped ($2)"
	RESULTS+=("skip  $1 ($2)")
}
summary() {
	echo
	echo "== summary"
	printf '%s\n' "${RESULTS[@]}"
}

# ---------------------------------------------------------------------------------------------------------------------
linux_build() {
	cmake -S "$ROOT" -B "$CI/linux" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$CI/linux" -j"$JOBS"
}

# VR server + libovr-opendesk test app on one GPU (short XDG_RUNTIME_DIR: Unix socket paths are limited)
linux_vr() {
	local node=$1 run
	run=$(mktemp -d /tmp/odci.XXXX)
	chmod 700 "$run"
	XDG_RUNTIME_DIR=$run OPENDESK_VR_RENDER_NODE=$node "$CI/linux/core/vr-server-test" 8 >"$run/server.log" 2>&1 &
	local server=$!
	sleep 2
	XDG_RUNTIME_DIR=$run OPENDESK_VR_RENDER_NODE=$node timeout 30 "$CI/linux/runtime/ovr-test-app" 4
	local app=$?
	wait $server
	local srv=$?
	cat "$run/server.log"
	rm -rf "$run"
	[ $app -eq 0 ] && [ $srv -eq 0 ]
}

linux_encode() {
	local node=$1 out
	out=$(mktemp)
	for codec in 1 2 10; do
		OPENDESK_RENDER_NODE=$node "$CI/linux/core/video-test" encode 1280 720 60 "$out" "$codec" limited pre+vbaq || { rm -f "$out"; return 1; }
	done
	rm -f "$out"
}

run_linux() {
	step linux-build linux_build
	if [ $GPU = 0 ]; then
		skip linux-gpu-tests "--no-gpu"
		return
	fi
	for node in /dev/dri/renderD*; do
		[ -e "$node" ] || continue
		local vendor
		vendor=$(cat "/sys/class/drm/$(basename "$node")/device/vendor" 2>/dev/null)
		local name
		name=$(basename "$node")-${vendor#0x}
		step "linux-vr-$name" linux_vr "$node"
		step "linux-encode-$name" linux_encode "$node"
	done
}

# ---------------------------------------------------------------------------------------------------------------------
windows_sysroot() {
	[ -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++" ] || { echo "llvm-mingw not found at $LLVM_MINGW (set LLVM_MINGW)"; return 1; }
	python3 -I "$ROOT/tools/windows/fetch-sysroot.py" "$TOOLCHAIN/win-sysroot" || return 1
	[ -f "$TOOLCHAIN/win-sysroot/wil/include/wil/resource.h" ] ||
		git clone -q --depth 1 https://github.com/microsoft/wil.git "$TOOLCHAIN/win-sysroot/wil"
}

windows_build() {
	cmake -S "$ROOT" -B "$CI/windows" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/windows/llvm-mingw.cmake" \
		-DOPENDESK_WIN_SYSROOT="$SYSROOT" -DLLVM_MINGW="$LLVM_MINGW" && cmake --build "$CI/windows" -j"$JOBS"
}

wine_env() {
	export WINEPREFIX=$CI/wineprefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
	export WINEPATH="$(winepath -w "$SYSROOT/bin" 2>/dev/null);$(winepath -w "$LLVM_MINGW/x86_64-w64-mingw32/bin" 2>/dev/null)"
	[ -d "$WINEPREFIX" ] || wineboot -i >/dev/null 2>&1
}

windows_ipc() {
	wine_env
	wine "$CI/windows/core/vr-server-test.exe" 10 >"$LOG/windows-ipc-server.txt" 2>&1 &
	local server=$!
	sleep 4
	timeout 30 wine "$CI/windows/core/vr-test-client.exe" 4
	local client=$?
	wait $server
	local srv=$?
	cat "$LOG/windows-ipc-server.txt"
	[ $client -eq 0 ] && [ $srv -eq 0 ]
}

windows_openxr() {
	wine_env
	# the loader ignores XR_RUNTIME_JSON in (Wine's) elevated processes: register the build's runtime in the CI prefix
	wine reg add 'HKLM\SOFTWARE\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ \
		/d "$(winepath -w "$CI/windows/runtime/opendesk_xr.json")" /f /reg:64 >/dev/null 2>&1
	wine "$CI/windows/core/vr-server-test.exe" 15 >"$LOG/windows-openxr-server.txt" 2>&1 &
	local server=$!
	sleep 4
	timeout 30 wine "$CI/windows/runtime/xr-system-test.exe"
	local app=$?
	kill $server 2>/dev/null
	wait $server 2>/dev/null
	[ $app -eq 0 ]
}

XRIZER_SRC=${OPENDESK_XRIZER_SRC:-$(cd "$ROOT/.." && pwd)/xrizer-opendesk}
windows_xrizer() {
	LLVM_MINGW=$LLVM_MINGW OPENDESK_WIN_SYSROOT=$SYSROOT bash "$ROOT/tools/windows/build-xrizer.sh" "$XRIZER_SRC" "$CI/xrizer-win"
}

run_windows() {
	step windows-sysroot windows_sysroot
	step windows-build windows_build
	if [ -f "$XRIZER_SRC/Cargo.toml" ] && command -v cargo >/dev/null; then
		step windows-xrizer windows_xrizer
	else
		skip windows-xrizer "no xrizer source at $XRIZER_SRC (or no cargo)"
	fi
	if ! command -v wine >/dev/null; then
		skip windows-wine-tests "wine not installed"
		return
	fi
	step windows-ipc-wine windows_ipc
	step windows-openxr-wine windows_openxr
}

# ---------------------------------------------------------------------------------------------------------------------
package_windows() {
	OPENDESK_WIN_SYSROOT=$SYSROOT LLVM_MINGW=$LLVM_MINGW OPENDESK_XRIZER_WIN=$CI/xrizer-win \
		bash "$ROOT/tools/package-windows.sh" "$CI/windows"
}

installer_windows() {
	(cd "$ROOT/tools/windows" && makensis -DAPP_VERSION="${OPENDESK_VERSION:-1.0}" -DSTAGE_DIR="../../dist/OpenDesk" installer.nsi)
}

run_package() {
	[ -f "$CI/windows/streamer/opendesk-streamer.exe" ] || run_windows
	step package-windows package_windows
	if command -v makensis >/dev/null; then
		step installer-windows installer_windows
	else
		skip installer-windows "makensis not installed"
	fi
	if [ -x "$ROOT/tools/build-appimage.sh" ] && command -v docker >/dev/null; then
		step appimage-linux bash "$ROOT/tools/build-appimage.sh"
	else
		skip appimage-linux "tools/build-appimage.sh needs docker"
	fi
}

for s in "${STAGES[@]}"; do
	echo "== $s"
	"run_$s"
done
summary
