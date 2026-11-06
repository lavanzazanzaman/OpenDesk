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
runtime.

## Face tracking definitions

The Unified Expressions and VRCFaceTracking v2 avatar parameter definitions
used by `core/engine/face_tracking.cpp` are based on
[VRCFaceTracking](https://github.com/benaclejames/VRCFaceTracking) by
benaclejames and contributors, Apache-2.0.

## AMD FidelityFX CAS and FSR 1

`third_party/FidelityFX` holds `ffx_a.h`, `ffx_cas.h` and `ffx_fsr1.h` from
[FidelityFX-CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) and
[FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR), used by the Windows runtime's sharpening and
upscaling shaders. Copyright (c) 2020-2021 Advanced Micro Devices, Inc., MIT license (see the `LICENSE-*.txt` files
there).

## Windows Implementation Libraries (WIL)

The Windows runtime is compiled with [WIL](https://github.com/microsoft/wil) headers (fetched at build time, not part
of this repository). Copyright (c) Microsoft Corporation, MIT license.
