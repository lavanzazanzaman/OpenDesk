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

#include "linux_compat.h"

#include <vulkan/vulkan.h>

#include <OVR_CAPI.h>
#include <OVR_CAPI_Vk.h>
#include <OVR_Math.h>

#include "OVR_Ext.h"

#define XR_NO_PROTOTYPES
#define XR_USE_TIMESPEC
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

#include "meta_body_tracking_full_body.h"
#include "meta_body_tracking_fidelity.h"
#include "meta_body_tracking_calibration.h"

#include <openxr/openxr_loader_negotiation.h>

// OpenXR utilities (OpenXR-MixedReality Shared/XrUtility; XrMath uses DirectXMath)
#include <XrError.h>
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
