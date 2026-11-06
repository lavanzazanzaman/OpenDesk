# Windows test checklist

What to verify on a real Windows 10/11 PC. Everything here builds and passes the local CI (`tools/ci.sh`); under Wine
only the VR pipe and the OpenXR loader → OpenDesk XR path can run (Wine has no shared D3D11 textures or fences), so
the rest needs real hardware.

## Setup

1. Copy `dist/OpenDesk` (from `tools/ci.sh package`) to the Windows PC, or build the installer with `makensis` and
   install it (the installer also makes OpenDesk XR the active OpenXR runtime - test both ways if you can).
2. Start `OpenDesk.exe`. Open **System check**: every row should be `[ok]` except optional ones.
3. Logs to send when something fails:
   - streamer: `%LOCALAPPDATA%\cache\OpenDesk\streamer.log`
   - OpenDesk XR (per game): `%LOCALAPPDATA%\OpenDesk\OpenXR.log`
   - xrizer (OpenVR games): `%LOCALAPPDATA%\xrizer\xrizer.txt`
   - game launches: `%APPDATA%\OpenDesk\logs\game-launch.log`

Test on both GPUs where possible (NVIDIA and AMD): the encoder paths differ.

## 1. Desktop streaming (regression check)

- [ ] Headset finds the PC, pairs, desktop shows; mouse, keyboard, touch, audio and mic work.
- [ ] Codecs: H.264, HEVC, HEVC 10-bit, AV1 (AV1 was fixed for the headset's decoder: no artifacts/black screen).
- [ ] The **Adaptive quantization** and **2-Pass encoding** options now appear on Windows; streamer log line
      `[encode] ... 2-pass, AQ on <your GPU>` when enabled.
  - AMD: `h264_amf`/`hevc_amf` with 2-Pass/AQ on - image quality in dark scenes, no stutter.
  - NVIDIA: same with NVENC; no keyframe "pulsing" every 2 seconds anymore (intra refresh).
- [ ] Laptop / two GPUs: the log names the GPU you expect, and a monitor on the second GPU can be streamed.
- [ ] Desktop frames stay on the GPU: the format line says `GPU (Desktop Duplication -> D3D11 video processor)`.
      Check colours (no washed-out or crushed blacks in limited and full range), the mouse pointer (normal, text
      cursor, resize arrows, a game's custom cursor), and CPU use compared to before.

## 2. OpenXR games (OpenDesk XR)

- [ ] Installed with the installer: no UAC prompt at all. OpenDesk XR becomes the OpenXR runtime when the streamer
      starts and the previous one comes back when it exits (check `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`).
      `opendesk-service.exe --call status` says `active` while the streamer runs.
- [ ] Without the installer (copied folder): one UAC prompt; afterwards System check → OpenXR runtime `[ok]`.
- [ ] Enable OpenDesk XR's mirror window: it shows the game's view (not black).
- [ ] A **D3D11** OpenXR game (most Unity/Unreal titles): starts, headset switches to VR, image correct per eye
      (no swapped/mirrored eyes), head tracking, controllers, haptics, recenter.
- [ ] A **D3D12** OpenXR game.
- [ ] An **OpenGL** and a **Vulkan** OpenXR game if you have one.
- [ ] Streamer log while playing: `[vr-ipc] OpenXR app connected`, `6 buffers ... OPAQUE_WIN32`, and the encoder line:
  - NVIDIA: `CUDA input` (zero copy)
  - AMD: `GPU copy into D3D11 frames` (if it says `GPU readback`, send the log: the GPU copy didn't work there)
- [ ] Settings on the VR page apply: FOV tangents, render resolution, sharpening, FOV stencil.
- [ ] Synchronous Spacewarp (headset setting) on/off.
- [ ] Eye-tracked headset (Quest Pro): foveated streaming, eye/face tracking in a game that uses it; VRChat face
      tracking (OSC).
- [ ] Hand tracking and body tracking in a game that supports them.
- [ ] Unity games built with the Oculus/OVRPlugin XR plugin (they check for the Oculus runtime first).
- [ ] **Boost game priority**: log line `render thread ... at highest priority`.
- [ ] Quit the game: headset returns to the desktop; start another game right after.
- [ ] Games tab in the headset: launch a Steam VR game and an Oculus game from there.

## 3. OpenVR games without SteamVR (xrizer)

- [ ] With SteamVR **not running**, start an OpenVR game (e.g. from the Steam library). Log line
      `[openvr] xrizer ... is the OpenVR runtime`.
- [ ] D3D11 OpenVR game (most of them): image, tracking, controllers (bindings), haptics.
- [ ] An OpenGL OpenVR game if available.
- [ ] Multisampled games (some older titles) and games with overlays/dashboards.
- [ ] Close the streamer: `%LOCALAPPDATA%\openvr\openvrpaths.vrpath` points back at SteamVR (restored).
- Not supported yet: D3D12 OpenVR games (xrizer rejects their textures) and 32-bit OpenVR games.

## 4. DXVK per game

- [ ] VR page → DXVK list shows your VR games. Tick one D3D11 game: log `[dxvk] <game>: DXVK placed in ...`, and
      `d3d11.dll`, `dxgi.dll`, a hidden `.opendesk-dxvk` appear next to its .exe.
- [ ] Start it in VR: OpenXR.log shows the game on DXVK; streamer log `[app ...] DXVK device: compositing on its
      Vulkan device`. Image, performance and frame pacing compared to without DXVK.
- [ ] Untick it: DLLs and marker removed (log `DXVK removed`). A game folder that already had its own d3d11.dll is
      skipped and left untouched.
- [ ] "Use DXVK for VR games by default" toggles every game without its own setting.
- [ ] A game installed under Program Files (may need the streamer run as administrator - the log says so).

## 5. Virtual monitors (optional)

- [ ] Install the [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver); System check
      shows it.
- [ ] In the headset: Add monitor / Remove monitor, Extra display, portrait toggle.
- [ ] After disconnecting, the driver's monitor count is back to what it was.
- [ ] The streamer stays responsive while a monitor is added (the headset keeps streaming, then the new screen joins).

## 6. Crash reports

- [ ] `OPENDESK_TEST_CRASH=1` before starting OpenDesk.exe: it crashes on purpose; `streamer.log` ends with
      `--- crash: exception 0xc0000005` and a stack, and `streamer-crash.dmp` is next to it.

## Known gaps

- Accessibility controller emulation (needs Microsoft's GameInput SDK, not available to this toolchain).
- The built-in VR test pattern (`OPENDESK_VR_TEST_PATTERN`) is a Linux development aid.
