# Compatibility checklist (stock upstream headset app 1.34.22 / 1.34.23)

Everything the headset can change or ask for, and what the Linux streamer does with it.
Status: ✅ works · 🟡 partial · 🕶 VR stage (OpenDesk XR port) · ⏳ not yet · — not applicable on Linux.

The tables below describe the Linux backend. The Windows backend implements the
same protocol and settings for desktop streaming; the differences are listed in
[Windows backend](#windows-backend) at the end.

## Settings the headset syncs to the streamer

### SharedUserSettings (headset → streamer)
| Setting | Status | Streamer behaviour |
|---|---|---|
| DesktopBitrateLimit | ✅ | `lerp(4 Mbps, max, limit)`, max 120 Mbps (Quest 1: 40, Pico Space Pro: 200); live encoder restart |
| IncreaseVideoNominalRange | ✅ | full-range YUV + VUI flag (else limited), live |
| MicPassthrough | ✅ | headset-side switch; mic packets → "OpenDesk Microphone" |
| MicVolume | ✅ | gain `lerp(0.1, 2.0, MicVolume)` like the Windows streamer, live |
| EmulateGamepad | ✅ | headset-side; resulting gamepad state → virtual Xbox 360 / DualShock 4 pad |
| VRFramerate, VRGraphicsQuality, VRBitrateLimit | ✅ | VR fps, eye resolution (headset table), bitrate lerp(10 Mbps, 200 Mbps Quest 3) |
| Gamma | ✅ | applied in libovr-vd's compositor |
| UseOptimalResolution | ✅ | VR eye resolution from the headset table |
| ForwardTrackingData, EmulateTrackers, EmulateIndexControllers | ✅ | OpenDesk XR: body tracking (XR_FB/META full body, skeleton), emulated Vive trackers (XR_HTCX), Index controller emulation |
| ExtraDisplay | ✅ | KWin virtual output (same size, scale and refresh rate as the main monitor) becomes the main screen, removed when off / on disconnect |
| UseMultiModal | ✅ | headset-side; hands and controllers both arrive in FrameData |

### SharedMobileSettings (headset → streamer)
| Setting | Status | Streamer behaviour |
|---|---|---|
| DesktopFramerate | ✅ | encoder fps + PipeWire maxFramerate, live |
| MeasuredBandwidth | ✅ | caps the desktop bitrate when "Automatically adjust bitrate" is on |
| HmdType | ✅ | bitrate cap, headset name in ABOUT (Xenko.VR.HmdType names) |
| Resolution, IPD, FovLeft/FovRight | 🟡 | desktop: informational; VR: eye poses/FOV (used by the VR test grid) |
| UseAnnexB | ✅ | always Annex-B (the headset default) |
| Features | ✅ | hand / face / eye / body / full-body tracking support reported to OpenDesk XR |
| RefreshRate, IsPortrait, LeftHanded | ⏳ | informational |
| UseSpacewarp | ✅ | Synchronous Spacewarp: the app renders every other headset frame (libovr-vd pacing, ASW reported to OpenDesk XR) |
| FoveatedStreaming | ✅ | 4 streams (periphery + fovea per eye) around the averaged eye gaze; Fovea Size / Periphery Resolution in ADVANCED |
| VideosFolderPath, VideoPlaybackSpeed, IsVideoPaused | ✅ | Videos tab: browsing, pause, playback speed (atempo on the audio) |

### SharedStreamerSettings (streamer → headset)
| Setting | Status |
|---|---|
| HasVR, StreamingSource, ActiveRuntime | ✅ OpenDesk XR (ActiveRuntime 2) |
| ActiveCodec | ✅ codec actually encoding (after fallbacks) |
| AdapterName, AdapterIsAMD | ✅ GPU from lspci |
| AutoAdjustBitrate | ✅ |
| CanSwitchMonitor, CanAddMonitor, CanRemoveMonitor | ✅ Windows rules: switch if >1 monitor, add while below the headset's limit (Quest 3: 3), remove if >1 |
| CanLaunchSteamVR | — | SteamVR is not required: OpenVR games run on OpenDesk XR through xrizer |
| AllowMotionExtrapolation | ✅ | headset-side (default on) |
| DownloadState, DownloadProgress | — | not used (no Paste URL) |
| InputLanguage | ✅ KDE keyboard layout |

## Messages the headset sends
| Message | Status | Streamer behaviour |
|---|---|---|
| User/Mobile SettingsReload / PropertyChanged | ✅ | see above |
| StreamerSettingsPropertyChanged | ✅ | StreamingSource (VR toggle) |
| ForceKeyFrame, RestartStream | ✅ | last frame re-encoded as IDR immediately |
| UpdateRequired | ✅ | answered with UpdateUnavailable |
| ScreenshotToDesktop | ✅ | PNG saved to ~/Pictures/OpenDesk (tray notification) |
| DisposeGamepadEmulation | ✅ | virtual pad removed |
| Disconnect | ✅ | session teardown (+ lock if enabled) |
| SwitchMonitor, AddMonitor, RemoveMonitor, SetMonitorPortrait | ✅ | up to 4 screens: main as Frame packets, extras as Slice packets with Bounds; Add creates a KWin virtual output (mode of the main monitor, refresh via a KWin custom mode; stable names Upstream1..n so KWin keeps their placement) when every monitor is shown; portrait: virtual outputs recreated, physical ones rotated via kscreen-doctor until disconnect. Portal fallback (non-KDE): monitors picked in the dialog, no virtual ones |
| Clipboard* (text, image, files) | — | Windows streamer enables it only for phone/tablet/TV clients (HmdType 10–50); the Quest app never sends or handles these |
| RequestVideos, thumbnails, PlayVideo, SeekVideo, StopVideo, audio tracks, text subtitles (+offset) | ✅ | ~/Videos/OpenDesk as library; video passed through (H.264/HEVC/VP8/VP9/AV1), other codecs converted to H.264 on the fly, audio 48 kHz PCM, SRT/ASS/WebVTT/mov_text subtitles |
| DownloadVideo | ✅ | file copied to the headset over the data connection |
| PasteUrl, CancelPasteUrl | — | not used (Windows: YouTube download) |
| RequestGames, LaunchGame, ExitGame, thumbnails | ✅ | Games tab: Steam VR games + RiftLift |
| Recenter, BoundaryDataChanged | ✅ | VR |
| LaunchStreamVR, ExitSteamVR | — | no-op: OpenVR games run on OpenDesk XR through xrizer |
| SkeletonDataChanged | ✅ | body tracking skeleton to OpenDesk XR (xrGetBodySkeletonFB) |
| ToggleDictation | — | not used (Windows: voice typing) |
| ChangeLanguage | ✅ | next KDE keyboard layout |

## Windows Streamer options (OPTIONS / ADVANCED / BINDINGS; no MEDIA tab on Linux)
| Option | Status | Linux implementation |
|---|---|---|
| Preferred Codec | ✅ | Automatic = HEVC 10-bit (else H.264), H.264, H.264+ (= H.264 on desktop), HEVC, HEVC 10-bit, AV1, AV1 10-bit; fallback chain if the GPU can't |
| Adaptive quantization | ✅ | NVIDIA: spatial AQ; AMD: VBAQ via the Mesa VAAPI quality level (bit 4) |
| 2-Pass encoding | ✅ | NVIDIA: NVENC `multipass=qres` analysis pass; AMD: pre-encode pass via the VAAPI quality level (bit 3), ~0.3 ms/frame |
| OpenXR Runtime | ✅ | OpenDesk XR (VDXR-derived); OpenVR games run through xrizer |
| Gamepad Emulation | ✅ | Automatic (headset's model) / Xbox 360 / DualShock 4, rumble forwarded |
| Audio Streaming | ✅ | computer only / VR headset only (virtual sink or mute) / both |
| Use virtual audio driver | ✅ | "OpenDesk Audio" sink made default while connected |
| VoiceMeeter mode | — | Windows-only |
| Allow remote connections | — | not offered: needs upstream's cloud registry and relay (home network only) |
| Encrypt local traffic | ✅ | AesStream on data/video/audio |
| Automatically adjust bitrate | ✅ | |
| Start with Windows → Start with login | ✅ | XDG autostart entry |
| Start minimized in tray | ✅ | |
| Use touch input | ✅ | laser pointers as multitouch touchscreen, else mouse |
| Lock computer on disconnect | ✅ | `loginctl lock-session` |
| Auto-select microphone | ✅ | opendesk_mic made default input while in use |
| Ask for computer access | ✅ | pairing prompt |
| ADVANCED (FOV tangents, OpenDesk XR resolution, fovea size, periphery, sharpening, FOV stencil, game priority) | ✅ | OpenDesk XR on libovr-vd |
| BINDINGS | ✅ | GlobalShortcuts portal → headset actions |
| MEDIA: screenshots folder | — | fixed: ~/Pictures/OpenDesk |
| MEDIA: videos folder | — | fixed: ~/Videos/OpenDesk (Videos tab root) |

## Capture backends
| Backend | When | Notes |
|---|---|---|
| KWin screen cast (`zkde_screencast_unstable_v1`) | Plasma, after the system check installs `~/.local/share/applications/opendesk-streamer.desktop` (`X-KDE-Wayland-Interfaces`) | no dialog, every output separately, virtual outputs; touchscreens mapped per monitor via KWin `outputName` |
| xdg-desktop-portal ScreenCast | other Wayland desktops | share dialog once (restore token), several monitors selectable |
| X11 (MIT-SHM + XRandR) | X11 sessions | monitors from XRandR, cursor drawn in, no virtual monitors |

## Windows backend

The Windows build (`WIN32`) shares the protocol, GUI, settings and input
encoding with the Linux build and replaces the platform pieces:

| Area | Windows implementation |
|---|---|
| Capture | DXGI Desktop Duplication (`win_capture.cpp`), one capture per streamed monitor, cursor composited in |
| Encoding | FFmpeg hardware backends in order NVENC → AMF → QSV → Media Foundation → libx264/libx265; BGRA converted to NV12/P010 with swscale |
| Audio | WASAPI loopback for desktop audio (`audio_win.cpp`), headset mic mixed to the selected render device |
| Input | `SendInput` for keyboard/mouse, Windows touch injection (`InjectTouchInput`) for the headset's laser pointers |
| Displays | Enumerate and switch physical monitors (`displays_win.cpp`); virtual displays are not created on Windows |
| Gamepad | ViGEm virtual Xbox 360 / DualShock 4 pad (`gamepad_win.cpp`) |
| Games tab | Steam libraries plus Oculus (Meta store) manifests from `HKLM\SOFTWARE\WOW6432Node\Oculus VR, LLC\Oculus` and the per-user `Libraries` registry keys |
| Launch | Oculus apps start through `OculusClient.exe`; Steam through `steam://launch/<id>/vr` |
| OpenXR runtime | Not built on Windows: desktop streaming only, VR streaming is planned |

Windows-only options (VoiceMeeter, virtual audio driver, KWin/portal capture)
are not shown or are replaced by their Windows equivalents.
