# OpenDesk

OpenDesk is an independent, open-source PC-side streamer for standalone VR
headsets. It speaks the same local-network streaming protocol as the stock
headset app, so you can use the headset app you already have — no patched
headset build and no cloud account.

The project is an independent clean-room implementation. **OpenDesk is unaffiliated with
Virtual Desktop Inc.** We only provide the PC-side streamer: it works only with
the paid Virtual Desktop app from Virtual Desktop Inc., and it is useless
without that app. Wire-protocol identifiers and the protocol XML namespace are
kept only where compatibility requires them.

## Download

Linux builds are published as a portable AppImage on the
[Releases](https://github.com/lavanzazanzaman/OpenDesk/releases) page. It runs on
any x86-64 Linux distribution with a Vulkan-capable GPU; no installation is
needed.

## What works

| Area | Linux | Windows |
|---|---|---|
| Discovery and pairing | ✅ | ✅ |
| Desktop streaming (H.264 / HEVC / AV1) | ✅ | ✅ |
| Multi-monitor and virtual displays | ✅ | ✅ (virtual monitors with the [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)) |
| Keyboard, mouse, touch, laser pointer | ✅ | ✅ |
| Desktop audio, headset microphone | ✅ | ✅ |
| Videos tab (local library and playback) | ✅ | ✅ |
| Games tab | ✅ | ✅ (Steam + Oculus) |
| OpenXR runtime ("OpenDesk XR": D3D11 / D3D12 / OpenGL / Vulkan games) | ✅ | 🧪 built, hardware testing pending |
| Face / eye / body tracking to VRChat | ✅ | ✅ |
| Spacewarp / foveated VR streaming | ✅ | 🧪 built, hardware testing pending |
| OpenVR games without SteamVR (xrizer) | ✅ | 🧪 built (D3D11 / OpenGL / Vulkan games), hardware testing pending |
| DXVK per game (Direct3D 11 on Vulkan) | — (Proton) | 🧪 built, hardware testing pending |

The Linux build is the primary target and is tested against the stock headset
app. The Windows build runs the same GUI/engine and now includes the VR server,
VR encoding (NVENC zero-copy through CUDA, AMF/QSV) and the OpenDesk XR runtime;
those still need testing on real Windows hardware. See
[`docs/compatibility.md`](docs/compatibility.md) for the protocol-level
details.

## Version compatibility

The headset app refuses a streamer whose minor version is newer than its own, and asks older streamers to update.
Headset apps from **1.34.25** send their own version when they search for computers; by default (**Automatic**,
Advanced tab) OpenDesk reports exactly that version back, so the headset neither refuses the PC nor asks for an update,
and gets every feature it supports. Older headset apps don't send a version and get **1.20.3**, the lowest version the
headset app accepts (every app from 1.20.3 onward connects).

Uncheck Automatic to always report the version in **Reported Streamer Version**, or launch with:

```
OPENDESK_STREAMER_VERSION=1.34.23 opendesk-streamer
```

The setting takes effect on the next connection. The headset's update prompt is declined automatically.

## Build

### Linux

Dependencies: CMake ≥ 3.25, Ninja, a C++20 compiler, Qt 6 (Widgets, Network,
DBus), FFmpeg, PipeWire, PulseAudio, libevdev, OpenSSL, Vulkan, EGL/GBM,
Wayland, and (optionally) X11.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/streamer/opendesk-streamer
```

Install:

```sh
sudo cmake --install build
```

A portable AppImage can be built with `tools/build-appimage.sh` (requires
Docker). Third-party code is vendored under `third_party/`; no submodules are
needed.

### Windows

Dependencies: MSYS2 (MinGW-w64), Qt 6, FFmpeg, CMake, Ninja, OpenSSL.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The Windows build streams the desktop at full frame rate using DXGI Desktop
Duplication plus a hardware encoder (NVENC → AMF → QSV → Media Foundation →
libx264/libx265), with WASAPI audio and SendInput keyboard/mouse/touch
injection. The Games tab lists Steam titles and Oculus (Meta store) apps
installed on the PC. The GitHub release workflow builds a portable ZIP and an
NSIS installer; the OpenXR runtime (VR streaming) is currently Linux-only.

## Network

Open these ports on the PC firewall:

- UDP 38850 — discovery
- TCP 38810–38840 — messaging, data, video, audio

Settings live in the platform config directory (`OpenDesk/` on Linux,
`%APPDATA%\OpenDesk` on Windows). The engine log is written to the platform
cache directory (`~/.cache/OpenDesk/streamer.log` on Linux).

## Project layout

| Path | Contents |
|---|---|
| `streamer/` | Qt 6 GUI |
| `core/` | Native streaming engine (capture, encode, input, audio, protocol) |
| `runtime/openxr/` | OpenDesk XR OpenXR runtime (derived from VDXR, MIT) |
| `runtime/libovr-opendesk/` | LibOVR-compatible layer over the streamer's VR IPC |
| `engine/` | Python reference implementation used by the test suite |
| `third_party/` | Vendored third-party code |
| `docs/` | Protocol and compatibility notes |

## License

OpenDesk is released under the MIT License. See [`LICENSE`](LICENSE) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for the licenses of
vendored components and derived work.

OpenDesk does not redistribute any Virtual Desktop artwork or binaries. UI
artwork ships with the project; the original binaries are never included.
