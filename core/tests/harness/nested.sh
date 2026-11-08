#!/bin/bash
# Isolated end-to-end test: private network namespace + private D-Bus + headless nested KWin + own XDG dirs.
# Nothing touches the real desktop, audio routing, config or network ports.
#   nested.sh WORKDIR SCENARIO [extra fake_headset env...]
#   SCENARIO: MONITORS | VIDEOS | GAMES | VR | VRCLIENT | OVRAPP | HELLOXR | AUTOVR | AUTOXR | XREXIT | OPENVR | REJOIN | FOVEATED | FACE
#             | XRBODY (body tracking through OpenDesk XR: joints, skeleton, emulated Vive trackers)
#             | XRPOSE (controller grip relative to aim, printed: xr-pose-probe)
#             | XRMASK (FOV stencil through OpenDesk XR: XR_KHR_visibility_mask; OPENDESK_TEST_NO_STENCIL=1: unchecked -> empty)
#             | X11 (an X11 session: rootful Xwayland 1280x720 as the desktop, MIT-SHM capture)
# Streamer log: WORKDIR/cache/OpenDesk/streamer.log
# OPENDESK_STREAMER_BIN: another streamer binary (e.g. an extracted AppImage's AppRun)
# Vulkan validation (every process): VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation [VK_LAYER_VALIDATE_SYNC=1 / VK_LAYER_GPUAV_ENABLE=1]
# OPENDESK_TEST_GAME_GPU / OPENDESK_TEST_STREAMING_GPU=<PCI address>: "Use two graphics cards" on
# OPENDESK_TEST_FIXED_PRESET=1: Adaptive encoder speed off (steady encode times with VR_MEASURE_SECONDS=12)
# OPENDESK_TEST_ADAPTIVE_SPEED=1: pretend NVENC is overloaded (adaptive preset steps + encoder re-open)
# HANDS=1 OPENDESK_TEST_HANDS=1 (OVRAPP): hand joints must come out in the controllers' space
# FACE: OPENDESK_VRCHAT_OSC_DIR / OPENDESK_FACE_OSC_PORT point the face tracking output at a test avatar config and port
# OPENDESK_TEST_PERIPHERY=0.33: foveated streaming periphery resolution (fraction of the eye)
# OPENDESK_TEST_RESOLUTION=0.67: OpenDesk XR Render Resolution (plain bilinear scaling, to compare)
# OPENDESK_TEST_FOVEA=0.1: Fovea Size (foveated streaming crop)
# OPENDESK_TEST_SOFT_FOVEA=1 (FOVEATED): Soft fovea edge on - checks the fovea's edge matches the periphery
# OPENDESK_TEST_BOOST=1: Boost game priority (the VR app's render thread through rtkit)
# OPENDESK_TEST_ENCRYPT=1: Encrypt local traffic (every connection AES, like over the internet)
# SPACEWARP=1 (OVRAPP / HELLOXR): the headset turns Synchronous Spacewarp on and off; the app's rate must follow
# OPENDESK_TEST_CODEC=N: the streamer's PreferredCodec (1 H.264, 2 HEVC, 6 HEVC 10-bit, 10 AV1)
# OPENDESK_TEST_TILED_VR_BUFFERS=1: VR buffers in a tiled layout like on NVIDIA (no linear GBM render buffers)
set -e
W=$(mkdir -p "$1" && cd "$1" && pwd); SCENARIO=$2
R=$(cd "$(dirname "$0")/../../.." && pwd)
TEST_PROTON=${OPENDESK_PROTON:-/usr/share/steam/compatibilitytools.d/proton-cachyos-slr/proton}
# OpenVR loader for the OpenVR test apps (games ship their own): SteamVR's copy, else the Steam client's
OPENVR_API=$HOME/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/libopenvr_api.so
[ -f "$OPENVR_API" ] || OPENVR_API=$HOME/.local/share/Steam/ubuntu12_64/libopenvr_api.so
mkdir -p $W/share/applications $W/cache $W/config/OpenDesk $W/videos/Movies
# short runtime dir: unix socket paths are limited to 108 bytes
RT=$(mktemp -d /tmp/vdrt.XXXXXX)
trap 'rm -rf $RT' EXIT
# KWin grants its screen-cast protocol to this binary
printf '[Desktop Entry]\nType=Application\nName=test streamer\nExec=%s\nX-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1\n' \
	"$(case "${OPENDESK_STREAMER_BIN:-}" in */AppRun) echo "${OPENDESK_STREAMER_BIN%/AppRun}/usr/bin/opendesk-streamer";; '') echo "$R/build/streamer/opendesk-streamer";; *) echo "$OPENDESK_STREAMER_BIN";; esac)" \
	> $W/share/applications/opendesk-streamer.desktop
if [ ! -f $W/videos/Movies/test.mkv ]; then
	printf '1\n00:00:01,000 --> 00:00:03,000\nHello {\\i1}subtitle{\\i0}\n\n2\n00:00:04,000 --> 00:00:05,500\nSecond line\n' > $W/s.srt
	ffmpeg -v error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=frequency=440:sample_rate=44100 -i $W/s.srt \
		-t 8 -map 0 -map 1 -map 2 -c:v libx264 -g 60 -c:a aac -ac 2 -c:s srt $W/videos/Movies/test.mkv
fi
# TRANSCODE: a codec the headset can't decode (MPEG-4 part 2, like DivX / Xvid AVIs)
if [ -n "${TRANSCODE_TEST:-}" ] && [ ! -f $W/videos/Movies/old.avi ]; then
	ffmpeg -v error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 6 \
		-c:v mpeg4 -q:v 4 -c:a mp3 $W/videos/Movies/old.avi 2>/dev/null || ffmpeg -v error -y -f lavfi -i testsrc2=size=640x360:rate=25 -t 6 -c:v mpeg4 -q:v 4 $W/videos/Movies/old.avi
fi
# Games tab: a RiftLift library with one game and a stand-in `riftlift` (first in PATH) that only waits
mkdir -p $W/bin $W/share/riftlift/games $W/share/riftlift/artwork/vd-test-game
[ -f $W/share/riftlift/artwork/vd-test-game/grid.png ] || ffmpeg -v error -y -f lavfi -i testsrc2=size=920x430 -frames:v 1 $W/share/riftlift/artwork/vd-test-game/grid.png
printf '{"slug": "vd-test-game", "name": "upstream Test Game", "app_id": "1234567890123", "app_key": "vd-test-game", "directory": "%s", "source": "meta"}\n' "$W" > $W/share/riftlift/games/vd-test-game.json
# worst case: the "game" is detached (like Wine's reparented processes) and only carries the launch marker, and the
# wrapper exits on SIGINT without cleaning up
# (the wrapper starts a marked "RiftLiftLauncher.exe /app <key>" like Proton's, which detaches the game)
printf '#!/bin/bash\necho "fake riftlift $*"\ntrap "exit 0" INT TERM\nRIFTLIFT_LAUNCH_ID=vdtest$$ bash -c "(setsid sleep 600 &); sleep 600; true" RiftLiftLauncher.exe /app vd-test-game &\nwait\n' > $W/bin/riftlift
chmod +x $W/bin/riftlift
# OPENDESK_TEST_XRIZER_ZIP=file (with OPENDESK_XRIZER= empty): the streamer has to fetch xrizer itself; this curl serves the file
if [ -n "${OPENDESK_TEST_XRIZER_ZIP:-}" ]; then
	printf '#!/bin/sh\nwhile [ $# -gt 0 ]; do [ "$1" = -o ] && { cp "%s" "$2"; exit 0; }; shift; done\nexit 1\n' "$OPENDESK_TEST_XRIZER_ZIP" > $W/bin/curl
	chmod +x $W/bin/curl
fi
FAKE=$SCENARIO; case $SCENARIO in VRCLIENT|OVRAPP|HELLOXR|OPENVRRENDER|OPENVRGL|PROTONVR|XROVERLAY|XROVERLAYFIRST|XROVERLAYALONE) FAKE=VR;; XREXIT|OPENVR|XRBODY|XRMASK|XRPOSE) FAKE=MONITORS;; X11) FAKE=X11;; AUTOXR) FAKE=AUTOVR;; esac
cat > $W/config/OpenDesk/streamer.json <<J
{"Accounts": {"1": ["testacct"]}, "CheckSetupAtStartup": false, "AudioStreaming": 0, "StartMinimized": true,
 "VideosRootPath": "$W/videos"${OPENDESK_TEST_HAND_POSE:+, \"HandPose\": $OPENDESK_TEST_HAND_POSE}${OPENDESK_TEST_CODEC:+, \"PreferredCodec\": $OPENDESK_TEST_CODEC}${OPENDESK_TEST_PERIPHERY:+, \"PeripheryScale\": $OPENDESK_TEST_PERIPHERY}${OPENDESK_TEST_RESOLUTION:+, \"RenderResolutionOverride\": $OPENDESK_TEST_RESOLUTION}${OPENDESK_TEST_FOVEA:+, \"FoveaSize\": $OPENDESK_TEST_FOVEA}${OPENDESK_TEST_ENCRYPT:+, \"EncryptLocalTraffic\": true}${OPENDESK_TEST_NO_STENCIL:+, \"UseFovStencil\": false}${OPENDESK_TEST_BOOST:+, \"BoostGamePriority\": true}${OPENDESK_TEST_SOFT_FOVEA:+, \"SoftFoveaEdge\": true}${OPENDESK_TEST_FIXED_PRESET:+, \"AdaptiveEncoderSpeed\": false}${OPENDESK_TEST_NO_AQ:+, \"EnableAQ\": false}${OPENDESK_TEST_TWO_PASS:+, \"Enable2Pass\": true}${OPENDESK_TEST_AMD_SPEED:+, \"AmdEncoderSpeed\": $OPENDESK_TEST_AMD_SPEED}${OPENDESK_TEST_QPA:+, \"StartMinimized\": false, \"StartMinimizedInTray\": false}${OPENDESK_TEST_GAME_GPU:+, \"DualGpu\": true, \"GameGpu\": \"$OPENDESK_TEST_GAME_GPU\", \"StreamingGpu\": \"$OPENDESK_TEST_STREAMING_GPU\"}}
J
rm -f $W/config/OpenDesk/engine.json
# the streamer's screenshots (ScreenshotToDesktop) go to $XDG_CONFIG_HOME/user-dirs.dirs' pictures folder, not ~/Pictures
mkdir -p $W/pictures
printf 'XDG_PICTURES_DIR="%s"\n' "$W/pictures" > $W/config/user-dirs.dirs
if [ "$SCENARIO" = FACE ]; then
	# VRChat "running" (the streamer only sends while a VRChat.exe process exists)
	(exec -a 'Z:\\VRChat\\VRChat.exe' sleep 40 &)
	# a VRChat avatar config (as VRChat writes it, with a BOM): a float, a binary-encoded and an eye-gaze parameter
	mkdir -p $W/osc/usr_test/Avatars
	printf '\xef\xbb\xbf{"id":"avtr_test","name":"Test","parameters":[
	 {"name":"FT/v2/JawOpen","input":{"address":"/avatar/parameters/FT/v2/JawOpen","type":"Float"},"output":{"address":"/avatar/parameters/FT/v2/JawOpen","type":"Float"}},
	 {"name":"FT/v2/MouthX1","input":{"address":"/avatar/parameters/FT/v2/MouthX1","type":"Bool"}},
	 {"name":"FT/v2/MouthX2","input":{"address":"/avatar/parameters/FT/v2/MouthX2","type":"Bool"}},
	 {"name":"FT/v2/MouthX4","input":{"address":"/avatar/parameters/FT/v2/MouthX4","type":"Bool"}},
	 {"name":"FT/v2/MouthXNegative","input":{"address":"/avatar/parameters/FT/v2/MouthXNegative","type":"Bool"}},
	 {"name":"FT/v2/EyeLeftX","input":{"address":"/avatar/parameters/FT/v2/EyeLeftX","type":"Float"}},
	 {"name":"FT/v2/TongueOut","input":{"address":"/avatar/parameters/FT/v2/TongueOut","type":"Float"}},
	 {"name":"FT/EyeTrackingActive","input":{"address":"/avatar/parameters/FT/EyeTrackingActive","type":"Bool"}},
	 {"name":"VelocityX","output":{"address":"/avatar/parameters/VelocityX","type":"Float"}}]}' > $W/osc/usr_test/Avatars/avtr_test.json
fi
cat > $W/inner.sh <<I
#!/bin/bash
export XDG_DATA_HOME=$W/share XDG_CACHE_HOME=$W/cache XDG_CONFIG_HOME=$W/config XDG_STATE_HOME=$W/state
unset WAYLAND_DISPLAY DISPLAY
kbuildsycoca6 >/dev/null 2>&1
kwin_wayland --virtual --no-lockscreen --no-global-shortcuts --socket vdnest --width 1600 --height 900 >$W/kwin.log 2>&1 &
K=\$!
for i in \$(seq 50); do [ -S /run/user/\$(id -u)/vdnest ] && break; sleep 0.1; done
export XDG_RUNTIME_DIR=$RT WAYLAND_DISPLAY=/run/user/1000/vdnest PIPEWIRE_RUNTIME_DIR=/run/user/1000 PULSE_SERVER=unix:/run/user/1000/pulse/native
[ "$SCENARIO" = VR ] && export OPENDESK_VR_TEST_PATTERN=1
# X11: an X server of its own (abstract socket only: nothing in the real /tmp/.X11-unix), a blue root window
if [ "$SCENARIO" = X11 ]; then
	Xwayland :77 -geometry 1280x720 -noreset -nolisten unix >$W/xwayland.log 2>&1 &
	for i in \$(seq 50); do DISPLAY=:77 xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
	DISPLAY=:77 xsetroot -solid '#3366aa' 2>/dev/null
	X11_ENV="DISPLAY=:77 XDG_SESSION_TYPE=x11 WAYLAND_DISPLAY="
fi
[ "$SCENARIO" = VR ] && export OPENDESK_TEST_VIDEO_SNDBUF=4194304 # like a Wi-Fi connection (loopback keeps it small)
env \$X11_ENV OPENDESK_VRCHAT_OSC_DIR=$W/osc OPENDESK_FACE_OSC_PORT=9123 OPENDESK_XRIZER=\${OPENDESK_XRIZER-$HOME/.local/share/opendesk/xrizer-build} PATH=$W/bin:\$PATH QT_QPA_PLATFORM=${OPENDESK_TEST_QPA:-offscreen} OPENDESK_QUIT_AFTER=60 ${OPENDESK_STREAMER_BIN:-$R/build/streamer/opendesk-streamer} > $W/streamer.out 2>&1 &
P=\$!
# OPENDESK_TEST_QPA=wayland + OPENDESK_TEST_KWIN_SCRIPT=file.js: the streamer's window on the nested desktop; the KWin
# script runs while the desktop streams (print() output: kwin.log)
if [ -n "${OPENDESK_TEST_KWIN_SCRIPT:-}" ]; then
	(sleep 5; I=\$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript $OPENDESK_TEST_KWIN_SCRIPT odtest); qdbus6 org.kde.KWin /Scripting/Script\$I org.kde.kwin.Script.run) &
fi
sleep 2
[ "$SCENARIO" = VRCLIENT ] && ($R/build/core/vr-test-client 30 > $W/client.out 2>&1 &)
# AUTOVR: two probing clients (connect, never render - like Proton's helpers) must not switch the headset to VR
[ "$SCENARIO" = AUTOVR ] && (sleep 4; $R/build/core/vr-test-client 1 probe; $R/build/core/vr-test-client 1 probe; sleep 7; $R/build/core/vr-test-client 12 > $W/client.out 2>&1) &
# AUTOXR: like AUTOVR with hello_xr - an OpenXR app renders nothing until visible, so the streamer has to switch the
# headset to VR on its frame loop, not on a submitted frame
[ "$SCENARIO" = AUTOXR ] && (sleep 4; $R/build/core/vr-test-client 1 probe; $R/build/core/vr-test-client 1 probe; sleep 7; sleep 12 | timeout 12 hello_xr -g Vulkan2 -vc Stereo > $W/client.out 2>&1) &
# OPENVRGL: an OpenVR game rendering with OpenGL (Vivecraft's path): needs an X server for GLX
if [ "$SCENARIO" = OPENVRGL ]; then
	Xwayland :79 -geometry 1280x720 -noreset -nolisten unix >$W/xwayland.log 2>&1 &
	for i in \$(seq 50); do DISPLAY=:79 xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
	(sleep 3; DISPLAY=:79 OPENDESK_OPENVR_API=$OPENVR_API $R/build/core/openvr-gl 12 > $W/client.out 2>&1) &
fi
[ "$SCENARIO" = OPENVRRENDER ] && (sleep 3; OPENDESK_OPENVR_API=$OPENVR_API $R/build/core/openvr-render 12 > $W/client.out 2>&1) &
[ "$SCENARIO" = PROTONVR ] && (sleep 3; mkdir -p $R/build/checks/proton-prefix; STEAM_COMPAT_DATA_PATH=$R/build/checks/proton-prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=\$HOME/.local/share/Steam SteamAppId=${OPENDESK_TEST_STEAM_APPID:-0} SteamGameId=${OPENDESK_TEST_STEAM_APPID:-0} PROTON_LOG=1 PROTON_LOG_DIR=$W timeout 40 "$TEST_PROTON" run "${OPENDESK_TEST_WINDOWS_EXE:-$R/build/checks/legacy-bin/legacy-openvr.exe}" ${OPENDESK_TEST_WINDOWS_ARGS:-12} > $W/client.out 2>&1) &
# OPENVR: an OpenVR app through the registered runtime (xrizer copy -> OpenDesk XR); records the registration
[ "$SCENARIO" = OPENVR ] && (sleep 3; cp $W/config/openvr/openvrpaths.vrpath $W/openvrpaths.during 2>/dev/null; OPENDESK_OPENVR_API=$OPENVR_API $R/build/core/openvr-smoke 6 > $W/client.out 2>&1) & C=\$!
[ "$SCENARIO" = OVRAPP ] && (OPENDESK_OVR_LOG=1 $R/build/runtime/ovr-test-app 30 > $W/client.out 2>&1 &)
[ "$SCENARIO" = XRMASK ] && (sleep 4; $R/build/runtime/xr-mask-test ${OPENDESK_TEST_NO_STENCIL:+off} > $W/client.out 2>&1) & [ "$SCENARIO" = XRMASK ] && C=\$!
[ "$SCENARIO" = XRBODY ] && (sleep 4; $R/build/runtime/xr-body-test > $W/client.out 2>&1) & [ "$SCENARIO" = XRBODY ] && C=\$!
[ "$SCENARIO" = XRPOSE ] && (sleep 4; bash -c "exec -a ${XRPOSE_ARGV0:-xr-pose-probe} $R/build/runtime/xr-pose-probe 6" > $W/client.out 2>&1) & [ "$SCENARIO" = XRPOSE ] && C=\$!
[ "$SCENARIO" = XREXIT ] && ($R/build/runtime/xr-exit-test > $W/client.out 2>&1; $R/build/runtime/xr-exit-test headless >> $W/client.out 2>&1 &)
# HELLOXR_HIDE_RUNTIME_DIR=1: the app gets another $XDG_RUNTIME_DIR (like Steam's pressure-vessel) -> abstract socket
[ -n "$HELLOXR_HIDE_RUNTIME_DIR" ] && APP_ENV="XDG_RUNTIME_DIR=$RT/app" && mkdir -p $RT/app
# HELLOXR_GFX=OpenGL: the app needs an X server (GLX); the streamer stays on Wayland
if [ "$SCENARIO" = HELLOXR ] && [ "${HELLOXR_GFX:-}" = OpenGL ]; then
	Xwayland :78 -geometry 1280x720 -noreset -nolisten unix >$W/xwayland.log 2>&1 &
	for i in \$(seq 50); do DISPLAY=:78 xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done
	APP_ENV="\$APP_ENV DISPLAY=:78"
fi
[ "$SCENARIO" = HELLOXR ] && (sleep 25 | env \$APP_ENV OPENDESK_OVR_LOG=1 timeout 25 hello_xr -g ${HELLOXR_GFX:-Vulkan2} -vc Stereo > $W/client.out 2>&1 &)
# FOVEATED: an eye-tracked headset streaming foveated (4 streams, gaze on the video connection) while hello_xr renders
[ "$SCENARIO" = FOVEATED ] && (sleep 30 | timeout 30 hello_xr -g Vulkan2 -vc Stereo > $W/client.out 2>&1 &)
# XROVERLAY: an XR_EXTX_overlay app joins a running game / XROVERLAYFIRST: it runs first and the game takes over /
# XROVERLAYALONE: it is the only app. WAYVR=<executable>: the real WayVR instead of the test app
OVERLAY="${WAYVR:-$R/build/runtime/xr-overlay-test 60 ${OVERLAY_ALPHA:-1}}"
[ "$SCENARIO" = XROVERLAY ] && (sleep ${OVERLAY_GAME_SECONDS:-30} | timeout ${OVERLAY_GAME_SECONDS:-30} hello_xr -g Vulkan2 -vc Stereo > $W/client.out 2>&1 &) && (sleep 5; timeout $((${OVERLAY_GAME_SECONDS:-30} - 8)) \$OVERLAY > $W/overlay.out 2>&1 &)
[ "$SCENARIO" = XROVERLAYFIRST ] && (timeout 32 \$OVERLAY > $W/overlay.out 2>&1 &) && (sleep 6; sleep ${OVERLAY_GAME_SECONDS:-24} | timeout ${OVERLAY_GAME_SECONDS:-24} hello_xr -g Vulkan2 -vc Stereo > $W/client.out 2>&1 &)
[ "$SCENARIO" = XROVERLAYALONE ] && (timeout 24 \$OVERLAY > $W/overlay.out 2>&1 &)
# REJOIN: hello_xr keeps running while the headset disconnects and reconnects (upstream app closed / crashed and reopened)
[ "$SCENARIO" = REJOIN ] && (sleep 30 | timeout 30 hello_xr -g Vulkan2 -vc Stereo > $W/client.out 2>&1 &)
env NOAUDIO=1 HOLD=1 VR_DUMP=$W/vr VR_DUMP_FOVEA=$W/fovea.txt $FAKE=1 ${@:3} $R/engine/.venv/bin/python $R/core/tests/fake_headset.py testacct > >(tee $W/fake.out)
rc=\$?
if [ "$SCENARIO" = REJOIN ]; then
	sleep 3
	env NOAUDIO=1 HOLD=1 VR_DUMP=$W/vr2 REJOIN=1 $R/engine/.venv/bin/python $R/core/tests/fake_headset.py testacct || rc=1
fi
[ "$SCENARIO" = PROTONVR ] && WINEPREFIX=$R/build/checks/proton-prefix/pfx "${TEST_PROTON%/proton}/files/bin/wineserver" -k
[ -n "\$C" ] && wait \$C
# Clients are bounded by their own duration/timeout; never pkill unrelated user apps.
 kill \$P 2>/dev/null; wait \$P; kill \$K; wait \$K 2>/dev/null
exit \$rc
I
chmod +x $W/inner.sh
if [ "$SCENARIO" = OPENVR ]; then
	# a user registration the streamer must take over and hand back
	mkdir -p $W/config/openvr
	printf '{"jsonid":"vrpathreg","runtime":["/opt/user-openvr-runtime"],"version":1}\n' > $W/config/openvr/openvrpaths.vrpath
	cp $W/config/openvr/openvrpaths.vrpath $W/openvrpaths.before
fi
# FOVEATED: an install of the removed foveated rendering layer (earlier versions), which the streamer deletes
[ "$SCENARIO" = FOVEATED ] && mkdir -p $W/share/vulkan/implicit_layer.d $W/share/opendesk/vulkan &&
	echo '{}' > $W/share/vulkan/implicit_layer.d/VkLayer_vd_foveated.json && touch $W/share/opendesk/vulkan/libVkLayer_vd_foveated.so
unshare -rn bash -c "ip link set lo up; dbus-run-session -- $W/inner.sh"
rc=$?
if [ "$SCENARIO" = FOVEATED ] && [ -n "${OPENDESK_TEST_SOFT_FOVEA:-}" ] && [ -s $W/fovea.txt ]; then
	# Soft fovea edge: the fovea's edge has to look like the periphery where the headset blends them
	sizes=$(grep -o "periphery [0-9]*x[0-9]*, fovea [0-9]*x[0-9]*, eye [0-9]*x[0-9]*" $W/fake.out 2>/dev/null | head -1 | grep -o "[0-9]*" | tr '\n' ' ')
	[ -n "$sizes" ] && { $R/engine/.venv/bin/python -I $R/core/tests/fovea_seam.py $W $sizes 200 || rc=1; }
fi
if [ "$SCENARIO" = FOVEATED ]; then
	# every stream decodes to the hello_xr scene (dark slate grey), the fovea crops included
	for s in 0 1 2 3; do
		y=$(ffmpeg -v error -i $W/vr-$s.bin -vf format=gray,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=- -f null - 2>/dev/null | grep -o 'YAVG=[0-9.]*' | tail -1 | cut -d= -f2)
		[ -n "$y" ] && [ "${y%.*}" -gt 30 ] && echo "PASS  stream $s decodes to the app's picture (Y avg $y)" || { echo "FAIL  stream $s: black or no picture (Y avg ${y:-none})"; rc=1; }
	done
fi
if [ "$SCENARIO" = REJOIN ]; then
	# hello_xr clears to dark slate grey: a black picture (Y ~16) means the app stopped rendering for the headset
	for d in vr vr2; do
		y=$(ffmpeg -v error -i $W/$d-0.bin -vf format=gray,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=- -f null - 2>/dev/null | grep -o 'YAVG=[0-9.]*' | tail -1 | cut -d= -f2)
		[ -n "$y" ] && [ "${y%.*}" -gt 30 ] && echo "PASS  $d: app picture in the stream (Y avg $y)" || { echo "FAIL  $d: black or no picture (Y avg ${y:-none})"; rc=1; }
	done
fi
if [ "$SCENARIO" = FOVEATED ]; then
	[ ! -e $W/share/vulkan/implicit_layer.d/VkLayer_vd_foveated.json ] && [ ! -e $W/share/opendesk/vulkan ] &&
		echo "PASS  old foveated rendering layer install removed" || { echo "FAIL  old foveated rendering layer files left behind"; rc=1; }
fi
case "$SCENARIO" in XROVERLAY|XROVERLAYFIRST|XROVERLAYALONE)
	cat $W/overlay.out
	for f in $W/vr-*.bin; do echo "$(basename $f): $(python3 -I $R/core/tests/overlay_check.py $f)"; done
	;;
esac
if [ "$SCENARIO" = XRMASK ]; then
	grep -E "PASS|FAIL" $W/client.out
	grep -q "ALL PASS" $W/client.out || { echo "FAIL  FOV stencil"; rc=1; }
fi
if [ "$SCENARIO" = XRBODY ]; then
	grep -E "PASS|FAIL" $W/client.out
	grep -q "ALL PASS" $W/client.out || { echo "FAIL  body tracking"; rc=1; }
fi
if [ "$SCENARIO" = OPENVR ]; then
	grep -q '"FAIL' $W/client.out && rc=1
	grep -q 'ALL PASS' $W/client.out || { echo "FAIL  OpenVR smoke test"; rc=1; }
	grep -q "$W/share/opendesk/xrizer" $W/openvrpaths.during && echo "PASS  xrizer registered while streaming" || { echo "FAIL  xrizer not registered"; rc=1; }
	cmp -s $W/openvrpaths.before $W/config/openvr/openvrpaths.vrpath && echo "PASS  user's OpenVR registration restored" || { echo "FAIL  openvrpaths.vrpath not restored"; rc=1; }
fi
exit $rc
