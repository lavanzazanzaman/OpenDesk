// MIT License
//
// Copyright(c) 2022-2024 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "log.h"
#include "runtime.h"
#include "utils.h"

// Implements the necessary support for the XR_KHR_win32_convert_performance_counter_time extension:
// https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#XR_KHR_win32_convert_performance_counter_time

namespace opendesk_openxr {

    using namespace opendesk_openxr::log;
    using namespace opendesk_openxr::utils;

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrConvertTimespecTimeToTimeKHR
    XrResult OpenXrRuntime::xrConvertTimespecTimeToTimeKHR(XrInstance instance,
                                                           const struct timespec* timespecTime,
                                                           XrTime* time) {
        TraceLoggingWrite(g_traceProvider,
                          "xrConvertTimespecTimeToTimeKHR",
                          TLXArg(instance, "Instance"),
                          TLArg(timespecTime->tv_sec, "PerformanceCounterSec"),
                          TLArg(timespecTime->tv_nsec, "PerformanceCounterNSec"));

        if (!has_XR_KHR_convert_timespec_time) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        double ovrTime = (double)timespecTime->tv_sec + (timespecTime->tv_nsec / 1e9f);
        ovrTime += m_ovrTimeFromTimeSpecTimeOffset;

        *time = ovrTimeToXrTime(ovrTime);

        TraceLoggingWrite(g_traceProvider, "xrConvertTimespecTimeToTimeKHR", TLArg(*time, "Time"));

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrConvertTimeToTimespecTimeKHR
    XrResult OpenXrRuntime::xrConvertTimeToTimespecTimeKHR(XrInstance instance,
                                                           XrTime time,
                                                           struct timespec* timespecTime) {
        TraceLoggingWrite(
            g_traceProvider, "xrConvertTimeToTimespecTimeKHR", TLXArg(instance, "Instance"), TLArg(time, "Time"));

        if (!has_XR_KHR_convert_timespec_time) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (time <= 0) {
            return XR_ERROR_TIME_INVALID;
        }

        double ovrTime = xrTimeToOvrTime(time);
        ovrTime -= m_ovrTimeFromTimeSpecTimeOffset;

        timespecTime->tv_sec = (time_t)ovrTime;
        double integerPart = (double)timespecTime->tv_sec;
        timespecTime->tv_nsec = (long)(modf(ovrTime, &integerPart) * 1e9);

        TraceLoggingWrite(g_traceProvider,
                          "xrConvertTimeToTimespecTimeKHR",
                          TLArg(timespecTime->tv_sec, "PerformanceCounterSec"),
                          TLArg(timespecTime->tv_nsec, "PerformanceCounterNSec"));

        return XR_SUCCESS;
    }

} // namespace opendesk_openxr
