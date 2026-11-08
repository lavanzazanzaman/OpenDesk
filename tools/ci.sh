#!/usr/bin/env bash
# Local CI: builds and tests run on this machine; nothing is built on GitHub.
#
#   tools/ci.sh [linux] [package] [--no-gpu] [--jobs N]
#
# With no stage named, runs linux and package.
#   linux   - configure + build (build-ci/linux), then with a GPU: VR server <-> libovr-opendesk Vulkan app on every
#             DRM GPU (DMA-BUF and, on NVIDIA, exportable Vulkan memory) and NVENC/VAAPI test encodes
#   package - the portable AppImage (tools/build-appimage.sh, needs Docker)
# Exit status: non-zero on the first failing step; a summary is printed at the end.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CI=$ROOT/build-ci
JOBS=$(nproc)
GPU=1
STAGES=()
while [ $# -gt 0 ]; do
	case "$1" in
	linux | package) STAGES+=("$1") ;;
	--no-gpu) GPU=0 ;;
	--jobs) JOBS=$2; shift ;;
	-h | --help) sed -n '2,10p' "$0"; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
	shift
done
[ ${#STAGES[@]} -eq 0 ] && STAGES=(linux package)
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
run_package() {
	if command -v docker >/dev/null; then
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
