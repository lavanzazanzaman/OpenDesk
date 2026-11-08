// MIT License, Copyright(c) 2022-2024 Matthieu Bucchianeri (see LICENSE). Linux port: see linux_compat.h.
#pragma once

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <shared_mutex>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

#ifdef _WIN32
// Windows (MinGW/clang cross-build): upstream's graphics core (D3D11, with D3D12 / OpenGL / Vulkan interop); the
// streamer is reached through libovr-opendesk exactly like on Linux
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#include <wrl.h>
#if !defined(_MSC_VER) && !defined(_MEMORY_)
#define _MEMORY_ // libc++'s <memory> is in: WIL enables its STL-based types (shared_handle) on MSVC's guard macro only
#endif
#include <wil/registry.h>
#include <wil/resource.h>
#include <TlHelp32.h>
#include <shlobj.h>
#include "trace_compat.h"
#include "win_hook.h" // DetourDllAttach without Detours (import table patching)

using Microsoft::WRL::ComPtr;

#include <d3d11_4.h>
#ifndef D3D11_MIP_LOD_BIAS_MIN // missing from MinGW's d3d11.h
#define D3D11_MIP_LOD_BIAS_MIN (-16.0f)
#define D3D11_MIP_LOD_BIAS_MAX (15.99f)
#endif
#include <d3d12.h>
#include <dxgi1_2.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>

#include <OVR_CAPI.h>
#include <OVR_CAPI_Audio.h>
#include <OVR_CAPI_D3D.h>
#include <OVR_CAPI_Vk.h>
#include <OVR_Math.h>

#include "OVR_Ext.h"

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_TIMESPEC
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_OPENGL
#else
#include "linux_compat.h"

#include <vulkan/vulkan.h>

#include <OVR_CAPI.h>
#include <OVR_CAPI_Vk.h>
#include <OVR_Math.h>

#include "OVR_Ext.h"

#define XR_NO_PROTOTYPES
#define XR_USE_TIMESPEC
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_OPENGL // (the Xlib binding is declared in linux_opengl.cpp: no X11 macros in every file)
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

#include "meta_body_tracking_full_body.h"
#include "meta_body_tracking_fidelity.h"
#include "meta_body_tracking_calibration.h"

#include <openxr/openxr_loader_negotiation.h>

// OpenXR utilities (OpenXR-MixedReality Shared/XrUtility; XrMath uses DirectXMath)
#include <XrError.h>
#ifndef _WIN32
namespace xr::detail {
    // XrError.h only defines the HRESULT checks on Windows
    inline HRESULT _CheckHResult(HRESULT hr, const char* originator = nullptr, const char* sourceLocation = nullptr) {
        if (FAILED(hr)) {
            throw std::runtime_error(_Fmt("HRESULT failure [%x]", hr) + (originator ? std::string(" ") + originator : "") +
                                     (sourceLocation ? std::string(" ") + sourceLocation : ""));
        }
        return hr;
    }
} // namespace xr::detail
#endif
#include <XrMath.h>
#include <XrSide.h>
#include <XrStereoView.h>
#include <XrToString.h>
#include <ScopeGuard.h>

// This header is not compatible with XR_NO_PROTOTYPES... We make a couple of symbols for now.
#define xrStringToPath(...) XR_ERROR_RUNTIME_FAILURE
#define xrPathToString(...) XR_ERROR_RUNTIME_FAILURE
#include <XrString.h>
#undef xrStringToPath
#undef xrPathToString

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include <cJSON.h>
