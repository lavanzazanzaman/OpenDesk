# Third-party notices

OpenDesk is MIT-licensed (see `LICENSE`). It contains or is derived from the
following components. Their licenses apply to those parts.

## Derived work

### VirtualDesktop-OpenXR (VDXR)

`runtime/openxr/` is derived from
[VirtualDesktop-OpenXR](https://github.com/mbucchia/VirtualDesktop-OpenXR) by
Matthieu Bucchianeri, used under the MIT License. The Linux/Vulkan port, the
streamer integration, and all OpenDesk-specific changes are part of OpenDesk.

### OpenXR runtime interfaces

`runtime/libovr-opendesk/` implements a subset of the Oculus LibOVR C API over
OpenDesk's streamer IPC. The public OVR headers under
`third_party/LibOVR/Include` are © Meta Platforms, Inc. and affiliates
(formerly Facebook Technologies, LLC) and are used under the terms shipped
with the Oculus PC SDK. They are included only for API compatibility.

## Vendored libraries

| Component | Version / source | License | Location |
|---|---|---|---|
| cJSON | Dave Gamble and contributors | MIT | `third_party/cJSON` |
| DirectXMath | Microsoft Corporation | MIT | `third_party/DirectXMath` |
| OpenVR | Valve Corporation | BSD-3-Clause | `third_party/openvr` |
| OpenXR headers | The Khronos Group | Apache-2.0 OR MIT | `third_party/openxr` |
| OpenXR-MixedReality | Microsoft Corporation | MIT | `third_party/OpenXR-MixedReality` |
| OpenGL headers | Khronos / SGI | MIT / SGI-B | `third_party/OpenGL` |

License texts are kept next to each component. If a component's license file
is missing, the copyright notice in its source files applies.

## Runtime dependencies (not vendored)

OpenDesk links against system libraries whose licenses are not covered by the
MIT license of this repository, including Qt 6 (LGPL-3.0/GPL), FFmpeg
(LGPL/GPL depending on build), OpenSSL (Apache-2.0), PipeWire (MIT), PulseAudio
(LGPL), libevdev (MIT), Vulkan (Apache-2.0), and Wayland (MIT). Binary releases
must comply with those licenses.

## xrizer

`tools/get_xrizer.sh` can download or build
[xrizer](https://github.com/Supreeeme/xrizer), which is GPL-3.0. xrizer is not
part of this repository; it is fetched separately and runs as its own OpenVR
runtime. The Windows build comes from a separate GPL-3.0 fork with a Direct3D 11
backend (built by `tools/windows/build-xrizer.sh`) and is shipped as its own
program, with the Khronos OpenXR loader (Apache-2.0) next to it.

## DXVK

The Windows package includes [DXVK](https://github.com/doitsujin/dxvk)'s
`d3d11.dll` and `dxgi.dll` (zlib license, `dxvk\LICENSE.txt`) for the per-game
DXVK option. `runtime/libovr-opendesk/dxvk_interop.h` declares DXVK's Vulkan
interop interfaces (zlib license).

## Face tracking definitions

The Unified Expressions and VRCFaceTracking v2 avatar parameter definitions
used by `core/engine/face_tracking.cpp` are based on
[VRCFaceTracking](https://github.com/benaclejames/VRCFaceTracking) by
benaclejames and contributors, Apache-2.0.
