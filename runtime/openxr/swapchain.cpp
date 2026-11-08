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

namespace opendesk_openxr {

    using namespace opendesk_openxr::log;
    using namespace opendesk_openxr::utils;

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurations
    XrResult OpenXrRuntime::xrEnumerateViewConfigurations(XrInstance instance,
                                                          XrSystemId systemId,
                                                          uint32_t viewConfigurationTypeCapacityInput,
                                                          uint32_t* viewConfigurationTypeCountOutput,
                                                          XrViewConfigurationType* viewConfigurationTypes) {
        std::vector<XrViewConfigurationType> types;
        types.push_back(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);

        TraceLoggingWrite(g_traceProvider,
                          "xrEnumerateViewConfigurations",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"),
                          TLArg(viewConfigurationTypeCapacityInput, "ViewConfigurationTypeCapacityInput"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        CHECK_MSG(ensureOVRSession(), "Failed to re-create OVR session\n");

        if (viewConfigurationTypeCapacityInput && viewConfigurationTypeCapacityInput < types.size()) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }

        *viewConfigurationTypeCountOutput = (uint32_t)types.size();
        TraceLoggingWrite(g_traceProvider,
                          "xrEnumerateViewConfigurations",
                          TLArg(*viewConfigurationTypeCountOutput, "ViewConfigurationTypeCountOutput"));

        if (viewConfigurationTypeCapacityInput && viewConfigurationTypes) {
            for (uint32_t i = 0; i < *viewConfigurationTypeCountOutput; i++) {
                viewConfigurationTypes[i] = types[i];
                TraceLoggingWrite(g_traceProvider,
                                  "xrEnumerateViewConfigurations",
                                  TLArg(xr::ToCString(viewConfigurationTypes[i]), "ViewConfigurationType"));
            }
        }

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetViewConfigurationProperties
    XrResult OpenXrRuntime::xrGetViewConfigurationProperties(XrInstance instance,
                                                             XrSystemId systemId,
                                                             XrViewConfigurationType viewConfigurationType,
                                                             XrViewConfigurationProperties* configurationProperties) {
        if (configurationProperties->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrGetViewConfigurationProperties",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"),
                          TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        CHECK_MSG(ensureOVRSession(), "Failed to re-create OVR session\n");

        if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO ||
                (m_apiMinor >= 1 &&
                 viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET)) {
                return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
            }
            return XR_ERROR_VALIDATION_FAILURE;
        }

        configurationProperties->viewConfigurationType = viewConfigurationType;
        configurationProperties->fovMutable = XR_TRUE;

        TraceLoggingWrite(g_traceProvider,
                          "xrGetViewConfigurationProperties",
                          TLArg(xr::ToCString(configurationProperties->viewConfigurationType), "ViewConfigurationType"),
                          TLArg(!!configurationProperties->fovMutable, "FovMutable"));

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurationViews
    XrResult OpenXrRuntime::xrEnumerateViewConfigurationViews(XrInstance instance,
                                                              XrSystemId systemId,
                                                              XrViewConfigurationType viewConfigurationType,
                                                              uint32_t viewCapacityInput,
                                                              uint32_t* viewCountOutput,
                                                              XrViewConfigurationView* views) {
        TraceLoggingWrite(g_traceProvider,
                          "xrEnumerateViewConfigurationViews",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"),
                          TLArg(viewCapacityInput, "ViewCapacityInput"),
                          TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        CHECK_MSG(ensureOVRSession(), "Failed to re-create OVR session\n");

        if (viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO ||
                (m_apiMinor >= 1 &&
                 viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET)) {
                return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
            }
            return XR_ERROR_VALIDATION_FAILURE;
        }

        if (viewCapacityInput && viewCapacityInput < xr::StereoView::Count) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }

        *viewCountOutput = xr::StereoView::Count;
        TraceLoggingWrite(
            g_traceProvider, "xrEnumerateViewConfigurationViews", TLArg(*viewCountOutput, "ViewCountOutput"));

        if (viewCapacityInput && views) {
            for (uint32_t i = 0; i < *viewCountOutput; i++) {
                if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) {
                    return XR_ERROR_VALIDATION_FAILURE;
                }

                // Lower the maximum on a low memory system.
                // Conformance testing is also create a number of very large textures, so we also lower the limit here.
                views[i].maxImageRectWidth = views[i].maxImageRectHeight =
                    !(m_isLowVideoMemorySystem || m_isConformanceTest) ? 16384 : 8192;

                // Per Direct3D 11 standard, "devices are required to support 4x MSAA for all render target formats, and
                // 8x MSAA for all render target formats except R32G32B32A32 formats.".
                // We could go and check every supported render target formats to find a possibly higher count, but we
                // do not bother.
                views[i].maxSwapchainSampleCount = 4;
                views[i].recommendedSwapchainSampleCount = 1;

                // Recommend the resolution with distortion accounted for.
                ovrFovPort fov;
                fov.UpTan = tan(m_cachedEyeFov[i].angleUp);
                fov.DownTan = tan(-m_cachedEyeFov[i].angleDown);
                fov.LeftTan = tan(-m_cachedEyeFov[i].angleLeft);
                fov.RightTan = tan(m_cachedEyeFov[i].angleRight);

                const float pixelDensity = m_supersamplingFactor * m_upscalingMultiplier;
                const ovrSizei viewportSize = ovr_GetFovTextureSize(
                    m_ovrSession, (i % xr::StereoView::Count) == 0 ? ovrEye_Left : ovrEye_Right, fov, pixelDensity);
                views[i].recommendedImageRectWidth =
                    xr::math::AlignTo<4>(std::min((uint32_t)viewportSize.w, views[i].maxImageRectWidth));
                views[i].recommendedImageRectHeight =
                    xr::math::AlignTo<4>(std::min((uint32_t)viewportSize.h, views[i].maxImageRectHeight));

                // XR_EPIC_view_configuration_fov
                for (auto* entry = reinterpret_cast<XrBaseOutStructure*>(views[i].next); entry; entry = entry->next) {
                    if (entry->type == XR_TYPE_VIEW_CONFIGURATION_VIEW_FOV_EPIC) {
                        auto* fovInfo = reinterpret_cast<XrViewConfigurationViewFovEPIC*>(entry);
                        fovInfo->recommendedFov = fovInfo->maxMutableFov = m_cachedEyeFov[i];
                    }
                }

                TraceLoggingWrite(g_traceProvider,
                                  "xrEnumerateViewConfigurationViews",
                                  TLArg(i, "ViewIndex"),
                                  TLArg(views[i].maxImageRectWidth, "MaxImageRectWidth"),
                                  TLArg(views[i].maxImageRectHeight, "MaxImageRectHeight"),
                                  TLArg(views[i].maxSwapchainSampleCount, "MaxSwapchainSampleCount"),
                                  TLArg(views[i].recommendedImageRectWidth, "RecommendedImageRectWidth"),
                                  TLArg(views[i].recommendedImageRectHeight, "RecommendedImageRectHeight"),
                                  TLArg(views[i].recommendedSwapchainSampleCount, "RecommendedSwapchainSampleCount"));
            }

            if (!m_loggedResolution) {
                Log("Recommended resolution: %ux%u (%.3f supersampling, %.3f upscaling)\n",
                    views[xr::StereoView::Left].recommendedImageRectWidth,
                    views[xr::StereoView::Left].recommendedImageRectHeight,
                    m_supersamplingFactor,
                    1 / m_upscalingMultiplier);
                m_loggedResolution = true;
            }
        }

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateSwapchainFormats

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateSwapchain

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySwapchain

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateSwapchainImages

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrAcquireSwapchainImage
    XrResult OpenXrRuntime::xrAcquireSwapchainImage(XrSwapchain swapchain,
                                                    const XrSwapchainImageAcquireInfo* acquireInfo,
                                                    uint32_t* index) {
        if (acquireInfo && acquireInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLXArg(swapchain, "Swapchain"));

        std::unique_lock lock(m_swapchainsMutex);

        if (!m_swapchains.count(swapchain)) {
            return XR_ERROR_HANDLE_INVALID;
        }

        Swapchain& xrSwapchain = *(Swapchain*)swapchain;

        // Check that we can acquire an image.
        if ((xrSwapchain.frozen && !m_allowStaticSwapchainsReuse) ||
            xrSwapchain.acquiredIndices.size() == xrSwapchain.ovrSwapchainLength) {
            return XR_ERROR_CALL_ORDER_INVALID;
        }

        // We don't query the image index from OVR: this is because LibOVR producer/consumer model works much
        // differently than OpenXR. We maintain our own index and there is logic in preprocessSwapchainImage() to ensure
        // we pass the correct image to the compositor.
        const int imageIndex = xrSwapchain.nextIndex;
        xrSwapchain.acquiredIndices.push_back(imageIndex);
        xrSwapchain.frozen = xrSwapchain.ovrDesc.StaticImage;
        xrSwapchain.nextIndex = imageIndex + 1;
        if ((int)xrSwapchain.nextIndex >= xrSwapchain.ovrSwapchainLength) {
            xrSwapchain.nextIndex = 0;
        }
        *index = imageIndex;

        TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLArg(*index, "Index"));

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrWaitSwapchainImage
    XrResult OpenXrRuntime::xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) {
        if (waitInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrWaitSwapchainImage",
                          TLXArg(swapchain, "Swapchain"),
                          TLArg(waitInfo->timeout, "Timeout"));

        std::unique_lock lock(m_swapchainsMutex);

        if (!m_swapchains.count(swapchain)) {
            return XR_ERROR_HANDLE_INVALID;
        }

        Swapchain& xrSwapchain = *(Swapchain*)swapchain;

        // Check an image is acquired but not waited.
        if (xrSwapchain.acquiredIndices.empty() || xrSwapchain.acquiredIndices.front() == xrSwapchain.lastWaitedIndex) {
            return XR_ERROR_CALL_ORDER_INVALID;
        }

        // We assume that our frame timing in xrWaitFrame() guaranteed availability of the next image. No wait.
        xrSwapchain.lastWaitedIndex = xrSwapchain.acquiredIndices.front();

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrReleaseSwapchainImage
    XrResult OpenXrRuntime::xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                    const XrSwapchainImageReleaseInfo* releaseInfo) {
        if (releaseInfo && releaseInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider, "xrReleaseSwapchainImage", TLXArg(swapchain, "Swapchain"));

        std::unique_lock lock(m_swapchainsMutex);

        if (!m_swapchains.count(swapchain)) {
            return XR_ERROR_HANDLE_INVALID;
        }

        Swapchain& xrSwapchain = *(Swapchain*)swapchain;

        // Check an image is acquired and waited.
        if (xrSwapchain.acquiredIndices.empty() || xrSwapchain.acquiredIndices.front() != xrSwapchain.lastWaitedIndex) {
            return XR_ERROR_CALL_ORDER_INVALID;
        }

        // Update the state of the swapchain.
        // We never commit images here: this is because LibOVR producer/consumer model works much differently than
        // OpenXR. We will perform swapchain commits in preprocessSwapchainImage().
        xrSwapchain.lastReleasedIndex = xrSwapchain.lastWaitedIndex;
        xrSwapchain.lastWaitedIndex = -1;
        xrSwapchain.dirty = true;
        xrSwapchain.acquiredIndices.pop_front();

        return XR_SUCCESS;
    }

} // namespace opendesk_openxr
