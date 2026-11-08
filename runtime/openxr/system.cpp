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

// From our OVR_CAPIShim.c fork.
OVR_PUBLIC_FUNCTION(ovrResult)
ovr_InitializeWithPathOverride(const ovrInitParams* inputParams, const wchar_t* overrideLibraryPath);
OVR_PUBLIC_FUNCTION(ovrResult) ovr_ReInitialize(const ovrInitParams* inputParams);

namespace opendesk_openxr {

    using namespace opendesk_openxr::log;
    using namespace opendesk_openxr::utils;
    using namespace xr::math;

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystem
    XrResult OpenXrRuntime::xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) {
        if (getInfo->type != XR_TYPE_SYSTEM_GET_INFO) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrGetSystem",
                          TLXArg(instance, "Instance"),
                          TLArg(xr::ToCString(getInfo->formFactor), "FormFactor"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (getInfo->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
            return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
        }

        // Hover Junkers uses OpenVR 1.0.3 exclusively. Proton also probes its
        // unrelated WineOpenXR bridge when DXVK starts; CachyOS Proton 11 then
        // asks Wine Vulkan for an unsupported OpenXR device extension and D3D11
        // creation fails. Suppress only that probe for this known OpenVR title.
        // xrizer's native OpenXR instance and other games remain available.
        const char* steamApp = std::getenv("SteamAppId");
        if (!steamApp) steamApp = std::getenv("SteamGameId");
        if (m_applicationName == "wineopenxr test instance" && steamApp && std::string_view(steamApp) == "380220") {
            Log("Hover Junkers compatibility: bypassing the unused WineOpenXR probe; OpenVR uses xrizer\n");
            return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        }

        // This is the latest point where we can defer initialization of LibOVR and the OVR session.
        if (!ensureOVRSession()) {
            m_cachedHmdInfo = {};
            return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        }

        m_systemCreated = true;

        *systemId = (XrSystemId)1;

        TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg((int)*systemId, "SystemId"));

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystemProperties
    XrResult OpenXrRuntime::xrGetSystemProperties(XrInstance instance,
                                                  XrSystemId systemId,
                                                  XrSystemProperties* properties) {
        if (properties->type != XR_TYPE_SYSTEM_PROPERTIES) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(
            g_traceProvider, "xrGetSystemProperties", TLXArg(instance, "Instance"), TLArg((int)systemId, "SystemId"));

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        CHECK_MSG(ensureOVRSession(), "Failed to re-create OVR session\n");

        XrSystemHandTrackingPropertiesEXT* handTrackingProperties = nullptr;
        XrSystemEyeGazeInteractionPropertiesEXT* eyeGazeInteractionProperties = nullptr;
        XrSystemEyeTrackingPropertiesFB* eyeTrackingProperties = nullptr;
        XrSystemFaceTrackingPropertiesFB* faceTrackingProperties = nullptr;
        XrSystemFaceTrackingProperties2FB* faceTrackingProperties2 = nullptr;
        XrSystemBodyTrackingPropertiesFB* bodyTrackingProperties = nullptr;
        XrSystemPropertiesBodyTrackingFullBodyMETA* fullBodyTrackingProperties = nullptr;
        XrSystemPropertiesBodyTrackingFidelityMETA* bodyTrackingFidelityProperties = nullptr;
        XrSystemHeadsetIdPropertiesMETA* headsetIdProperties = nullptr;

        XrBaseOutStructure* entry = reinterpret_cast<XrBaseOutStructure*>(properties->next);
        while (entry) {
            switch (entry->type) {
            case XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT:
                handTrackingProperties = reinterpret_cast<XrSystemHandTrackingPropertiesEXT*>(entry);
                break;
            case XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT:
                eyeGazeInteractionProperties = reinterpret_cast<XrSystemEyeGazeInteractionPropertiesEXT*>(entry);
                break;
            case XR_TYPE_SYSTEM_EYE_TRACKING_PROPERTIES_FB:
                eyeTrackingProperties = reinterpret_cast<XrSystemEyeTrackingPropertiesFB*>(entry);
                break;
            case XR_TYPE_SYSTEM_FACE_TRACKING_PROPERTIES_FB:
                faceTrackingProperties = reinterpret_cast<XrSystemFaceTrackingPropertiesFB*>(entry);
                break;
            case XR_TYPE_SYSTEM_FACE_TRACKING_PROPERTIES2_FB:
                faceTrackingProperties2 = reinterpret_cast<XrSystemFaceTrackingProperties2FB*>(entry);
                break;
            case XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB:
                bodyTrackingProperties = reinterpret_cast<XrSystemBodyTrackingPropertiesFB*>(entry);
                break;
            case XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META:
                fullBodyTrackingProperties = reinterpret_cast<XrSystemPropertiesBodyTrackingFullBodyMETA*>(entry);
                break;
            case XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FIDELITY_META:
                bodyTrackingFidelityProperties = reinterpret_cast<XrSystemPropertiesBodyTrackingFidelityMETA*>(entry);
                break;
            case XR_TYPE_SYSTEM_HEADSET_ID_PROPERTIES_META:
                headsetIdProperties = reinterpret_cast<XrSystemHeadsetIdPropertiesMETA*>(entry);
                break;
            }

            entry = reinterpret_cast<XrBaseOutStructure*>(entry->next);
        }

        properties->vendorId = m_cachedHmdInfo.VendorId;

        _snprintf_s(
            properties->systemName, sizeof(properties->systemName), _TRUNCATE, "%s", m_cachedHmdInfo.ProductName);
        properties->systemId = systemId;

        properties->trackingProperties.positionTracking = XR_TRUE;
        properties->trackingProperties.orientationTracking = XR_TRUE;

        static_assert(ovrMaxLayerCount >= XR_MIN_COMPOSITION_LAYERS_SUPPORTED);
        properties->graphicsProperties.maxLayerCount = ovrMaxLayerCount;
        properties->graphicsProperties.maxSwapchainImageWidth = 16384;
        properties->graphicsProperties.maxSwapchainImageHeight = 16384;

        TraceLoggingWrite(g_traceProvider,
                          "xrGetSystemProperties",
                          TLArg((int)properties->systemId, "SystemId"),
                          TLArg(properties->vendorId, "VendorId"),
                          TLArg(properties->systemName, "SystemName"),
                          TLArg(!!properties->trackingProperties.positionTracking, "PositionTracking"),
                          TLArg(!!properties->trackingProperties.orientationTracking, "OrientationTracking"),
                          TLArg(properties->graphicsProperties.maxLayerCount, "MaxLayerCount"),
                          TLArg(properties->graphicsProperties.maxSwapchainImageWidth, "MaxSwapchainImageWidth"),
                          TLArg(properties->graphicsProperties.maxSwapchainImageHeight, "MaxSwapchainImageHeight"));

        if (has_XR_EXT_hand_tracking && handTrackingProperties) {
            handTrackingProperties->supportsHandTracking =
                (m_supportsHandTracking || has_XR_EXT_hand_tracking_data_source) ? XR_TRUE : XR_FALSE;

            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystemProperties",
                              TLArg((int)properties->systemId, "SystemId"),
                              TLArg(!!handTrackingProperties->supportsHandTracking, "SupportsHandTracking"));
        }

        if (has_XR_EXT_eye_gaze_interaction && eyeGazeInteractionProperties) {
            eyeGazeInteractionProperties->supportsEyeGazeInteraction =
                (m_eyeTrackingType != EyeTracking::None) ? XR_TRUE : XR_FALSE;

            TraceLoggingWrite(
                g_traceProvider,
                "xrGetSystemProperties",
                TLArg(!!eyeGazeInteractionProperties->supportsEyeGazeInteraction, "SupportsEyeGazeInteraction"));
        }

        if (has_XR_FB_eye_tracking_social && eyeTrackingProperties) {
            eyeTrackingProperties->supportsEyeTracking = (m_eyeTrackingType == EyeTracking::Mmf) ? XR_TRUE : XR_FALSE;

            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystemProperties",
                              TLArg(!!eyeTrackingProperties->supportsEyeTracking, "SupportsEyeTracking"));
        }

        if (has_XR_FB_face_tracking && faceTrackingProperties) {
            faceTrackingProperties->supportsFaceTracking = m_supportsFaceTracking ? XR_TRUE : XR_FALSE;

            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystemProperties",
                              TLArg(!!faceTrackingProperties->supportsFaceTracking, "SupportsFaceTracking"));
        }

        if (has_XR_FB_face_tracking2 && faceTrackingProperties2) {
            faceTrackingProperties2->supportsVisualFaceTracking = faceTrackingProperties2->supportsAudioFaceTracking =
                m_supportsFaceTracking ? XR_TRUE : XR_FALSE;

            TraceLoggingWrite(
                g_traceProvider,
                "xrGetSystemProperties",
                TLArg(!!faceTrackingProperties2->supportsVisualFaceTracking, "SupportsVisualFaceTracking"),
                TLArg(!!faceTrackingProperties2->supportsAudioFaceTracking, "SupportsAudioFaceTracking"));
        }

        if (has_XR_FB_body_tracking && bodyTrackingProperties) {
            bodyTrackingProperties->supportsBodyTracking = m_supportsBodyTracking ? XR_TRUE : XR_FALSE;

            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystemProperties",
                              TLArg(!!bodyTrackingProperties->supportsBodyTracking, "SupportsBodyTracking"));
        }

        if (has_XR_META_body_tracking_full_body && fullBodyTrackingProperties) {
            fullBodyTrackingProperties->supportsFullBodyTracking = m_supportsFullBodyTracking ? XR_TRUE : XR_FALSE;

            TraceLoggingWrite(
                g_traceProvider,
                "xrGetSystemProperties",
                TLArg(!!fullBodyTrackingProperties->supportsFullBodyTracking, "SupportsFullBodyTracking"));
        }

        if (has_XR_META_body_tracking_fidelity && bodyTrackingFidelityProperties) {
            bodyTrackingFidelityProperties->supportsBodyTrackingFidelity =
                m_supportsFullBodyTracking ? XR_TRUE : XR_FALSE;

            TraceLoggingWrite(
                g_traceProvider,
                "xrGetSystemProperties",
                TLArg(!!bodyTrackingFidelityProperties->supportsBodyTrackingFidelity, "SupportsBodyTrackingFidelity"));
        }

        if (has_XR_META_headset_id && headsetIdProperties) {
            uint8_t uuid[] = {82, 80, 120, 165, 90, 171, 77, 201, 184, 2, 30, 189, 108, 124, 255, 244};
            memcpy(&headsetIdProperties->id, uuid, sizeof(uuid));
        }

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateEnvironmentBlendModes
    XrResult OpenXrRuntime::xrEnumerateEnvironmentBlendModes(XrInstance instance,
                                                             XrSystemId systemId,
                                                             XrViewConfigurationType viewConfigurationType,
                                                             uint32_t environmentBlendModeCapacityInput,
                                                             uint32_t* environmentBlendModeCountOutput,
                                                             XrEnvironmentBlendMode* environmentBlendModes) {
        // We only support immersive VR mode.
        static const XrEnvironmentBlendMode blendModes[] = {XR_ENVIRONMENT_BLEND_MODE_OPAQUE};

        TraceLoggingWrite(g_traceProvider,
                          "xrEnumerateEnvironmentBlendModes",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"),
                          TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"),
                          TLArg(environmentBlendModeCapacityInput, "EnvironmentBlendModeCapacityInput"));

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

        if (environmentBlendModeCapacityInput && environmentBlendModeCapacityInput < std::size(blendModes)) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }

        *environmentBlendModeCountOutput = (uint32_t)std::size(blendModes);
        TraceLoggingWrite(g_traceProvider,
                          "xrEnumerateEnvironmentBlendModes",
                          TLArg(*environmentBlendModeCountOutput, "EnvironmentBlendModeCountOutput"));

        if (environmentBlendModeCapacityInput && environmentBlendModes) {
            for (uint32_t i = 0; i < *environmentBlendModeCountOutput; i++) {
                environmentBlendModes[i] = blendModes[i];
                TraceLoggingWrite(g_traceProvider,
                                  "xrEnumerateEnvironmentBlendModes",
                                  TLArg(xr::ToCString(environmentBlendModes[i]), "EnvironmentBlendMode"));
            }
        }

        return XR_SUCCESS;
    }

    bool OpenXrRuntime::initializeOVR() {
        // libovr-opendesk (linked directly) talks to the OpenDesk streamer: its VR socket
        m_useOculusRuntime = false;
        if (!ovr_Detect(0).IsOculusServiceRunning) {
            OnceLog("OpenDesk Streamer is not running with VR enabled\n");
            return false;
        }
        ovrInitParams initParams{};
        initParams.Flags = ovrInit_RequestVersion | (has_XR_MND_headless ? ovrInit_Invisible : ovrInit_FocusAware);
        initParams.RequestedMinorVersion = OVR_MINOR_VERSION;
        CHECK_OVRCMD(ovr_Initialize(&initParams));
        Log("Using OpenDesk runtime\n");
        Log("OVR: %s\n", ovr_GetVersionString());
        m_isOVRLoaded = true;
        m_ovrSession = nullptr;
        return true;
    }

    void OpenXrRuntime::identifyOpenDesk() {
    }

    void OpenXrRuntime::enterVisibleMode() {
        ovrInitParams initParams{};
        initParams.Flags = ovrInit_RequestVersion | ovrInit_FocusAware;
        initParams.RequestedMinorVersion = OVR_MINOR_VERSION;
        CHECK_OVRCMD(ovr_ReInitialize(&initParams));
        TraceLoggingWrite(g_traceProvider, "OVR_ReInitialize");

        ovr_Destroy(m_ovrSession);
        m_ovrSession = nullptr;

        CHECK_MSG(ensureOVRSession(), "Failed to enter invisible mode\n");
    }

    bool OpenXrRuntime::ensureOVRSession() {
        if (m_ovrSession) {
            return true;
        }

        if (!m_isOVRLoaded) {
            if (!initializeOVR()) {
                return false;
            }
        }

        const ovrResult result = ovr_Create(&m_ovrSession, reinterpret_cast<ovrGraphicsLuid*>(&m_adapterLuid));
        TraceLoggingWrite(g_traceProvider, "OVR_Create", TLArg((int)result, "Result"));
        if (result == ovrError_NoHmd) {
            return false;
        }
        CHECK_OVRCMD(result);

        // Force the streamer to enter visible mode. This will make sure we transition our state machine later.
        ovrSessionStatus status{};
        CHECK_OVRCMD(ovr_GetSessionStatus(m_ovrSession, &status));

        // Tell the streamer that this is an OpenDesk XR session.
        if (!m_useOculusRuntime) {
            ovr_SetBool(m_ovrSession, "IsOpenDeskXR", true);
            ovr_SetBool(m_ovrSession, "IsOpenComposite", m_isOpenComposite);
        }

        // Calibrate the timestamp conversion.
        m_ovrTimeFromTimeSpecTimeOffset = INFINITY;
        for (int i = 0; i < 100; i++) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now); // XR_KHR_convert_timespec_time: CLOCK_MONOTONIC on Linux
            const double tsTime = (double)now.tv_sec + (now.tv_nsec / 1e9f);
            m_ovrTimeFromTimeSpecTimeOffset =
                std::min(m_ovrTimeFromTimeSpecTimeOffset, ovr_GetTimeInSeconds() - tsTime);
        }
        TraceLoggingWrite(g_traceProvider,
                          "ConvertTime",
                          TLArg(m_ovrTimeFromQpcTimeOffset, "OvrTimeFromQpcTimeOffset"),
                          TLArg(m_ovrTimeFromTimeSpecTimeOffset, "OvrTimeFromTimeSpecTimeOffset"));

        m_isLowVideoMemorySystem = false;
        initializeSystem();

        return true;
    }

    void OpenXrRuntime::initializeSystem() {
        // Query HMD properties.
        const ovrHmdDesc hmdInfo = ovr_GetHmdDesc(m_ovrSession);
        TraceLoggingWrite(g_traceProvider,
                          "OVR_HmdDesc",
                          TLArg((int)hmdInfo.Type, "Type"),
                          TLArg(hmdInfo.VendorId, "VendorId"),
                          TLArg(hmdInfo.ProductId, "ProductId"),
                          TLArg(hmdInfo.Manufacturer, "Manufacturer"),
                          TLArg(hmdInfo.ProductName, "ProductName"),
                          TLArg(hmdInfo.SerialNumber, "SerialNumber"),
                          TLArg(hmdInfo.FirmwareMinor, "FirmwareMinor"),
                          TLArg(hmdInfo.FirmwareMajor, "FirmwareMajor"),
                          TLArg(hmdInfo.Resolution.w, "ResolutionWidth"),
                          TLArg(hmdInfo.Resolution.h, "ResolutionHeight"),
                          TLArg(hmdInfo.DisplayRefreshRate, "DisplayRefreshRate"));

        // Detect if the device changed.
        if (std::string_view(m_cachedHmdInfo.SerialNumber) != hmdInfo.SerialNumber) {
            m_cachedHmdInfo = hmdInfo;
            Log("Device is: %s (%d)\n", m_cachedHmdInfo.ProductName, m_cachedHmdInfo.Type);

            // Try initializing the body and eye tracking data through the streamer.
            initializeBodyTrackingMmf();

            // We must latch the body tracking capabilities now, as they are not allowed to change later during the
            // lifetime of the system.
            m_eyeTrackingType = EyeTracking::None;
            if (!getSetting("simulate_eye_tracking").value_or(false)) {
                if (m_bodyState && ovr_GetBool(m_ovrSession, "SupportsEyeTracking", false)) {
                    m_eyeTrackingType = EyeTracking::Mmf;
                }
            } else {
                m_eyeTrackingType = EyeTracking::Simulated;
            }

            if (m_bodyState) {
                m_supportsHandTracking = ovr_GetBool(m_ovrSession, "SupportsHandTracking", false);
                m_supportsFaceTracking = ovr_GetBool(m_ovrSession, "SupportsFaceTracking", false);
                m_supportsBodyTracking = ovr_GetBool(m_ovrSession, "SupportsBodyTracking", false);
                m_supportsFullBodyTracking = ovr_GetBool(m_ovrSession, "SupportsFullBodyTracking", false);
                m_emulateViveTrackers = ovr_GetBool(m_ovrSession, "EmulateTrackers", false);
                m_emulateIndexControllers = ovr_GetBool(m_ovrSession, "EmulateIndexControllers", false);
            } else {
                m_supportsHandTracking = m_supportsFaceTracking = m_supportsBodyTracking = m_supportsFullBodyTracking =
                    m_emulateViveTrackers = m_emulateIndexControllers = false;
            }

            TraceLoggingWrite(g_traceProvider,
                              "OVR_ExtendedSupport",
                              TLArg(!!m_bodyState, "HasBodyState"),
                              TLArg((int)m_eyeTrackingType, "EyeTrackingType"),
                              TLArg(m_supportsHandTracking, "SupportsHandTracking"),
                              TLArg(m_supportsFaceTracking, "SupportsFaceTracking"),
                              TLArg(m_supportsBodyTracking, "SupportsBodyTracking"),
                              TLArg(m_supportsFullBodyTracking, "SupportsFullBodyTracking"),
                              TLArg(m_emulateViveTrackers, "EmulateViveTrackers"),
                              TLArg(m_emulateIndexControllers, "EmulateIndexControllers"));

            // Cache common information.
            m_displayRefreshRate = m_displayRefreshRateChanged = hmdInfo.DisplayRefreshRate;
            m_idealFrameDuration = m_predictedFrameDuration = 1.0 / hmdInfo.DisplayRefreshRate;
            m_cachedEyeInfo[xr::StereoView::Left] =
                ovr_GetRenderDesc(m_ovrSession, ovrEye_Left, m_cachedHmdInfo.DefaultEyeFov[ovrEye_Left]);
            m_cachedEyeInfo[xr::StereoView::Right] =
                ovr_GetRenderDesc(m_ovrSession, ovrEye_Right, m_cachedHmdInfo.DefaultEyeFov[ovrEye_Right]);

            for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
                m_cachedEyeFov[i].angleDown = -atan(m_cachedEyeInfo[i].Fov.DownTan);
                m_cachedEyeFov[i].angleUp = atan(m_cachedEyeInfo[i].Fov.UpTan);
                m_cachedEyeFov[i].angleLeft = -atan(m_cachedEyeInfo[i].Fov.LeftTan);
                m_cachedEyeFov[i].angleRight = atan(m_cachedEyeInfo[i].Fov.RightTan);

                TraceLoggingWrite(g_traceProvider,
                                  "OVR_EyeRenderInfo",
                                  TLArg(i == xr::StereoView::Left ? "Left" : "Right", "Eye"),
                                  TLArg(xr::ToString(m_cachedEyeInfo[i].HmdToEyePose).c_str(), "EyePose"),
                                  TLArg(xr::ToString(m_cachedEyeFov[i]).c_str(), "Fov"));
            }
        }

        m_supersamplingFactor = ovr_GetFloat(m_ovrSession, "RenderResolution", 1.f);
        m_upscalingMultiplier = getSetting("upscaling").value_or(100) / 100.f;

        TraceLoggingWrite(g_traceProvider,
                          "OpenDesk_Config",
                          TLArg(m_supersamplingFactor, "SupersamplingFactor"),
                          TLArg(m_upscalingMultiplier, "UpscalingMultiplier"));

        // Setup common parameters.
        // The streamer has a mode called "Stage Tracking" which requires us to use floor as the origin. For Oculus,
        // we use eye level for convenience.
        CHECK_OVRCMD(ovr_SetTrackingOriginType(
            m_ovrSession, !m_useOculusRuntime ? ovrTrackingOrigin_FloorLevel : ovrTrackingOrigin_EyeLevel));

        m_accessibilityHelper = CreateAccessibilityHelper(
            m_ovrSession, (programData / "accessibility.json").wstring(), m_applicationName, m_exeName);

        // When accessibility is enabled, we stop advertising hand joints as it may conflict with the use of the virtual
        // controllers.
        if (m_accessibilityHelper && (m_accessibilityHelper->IsControllerEmulated(xr::Side::Left) ||
                                      m_accessibilityHelper->IsControllerEmulated(xr::Side::Right))) {
            m_supportsHandTracking = false;
        }
    }

    // Hands, eyes, face and body come with the headset's FrameData through libovr-opendesk
    void OpenXrRuntime::initializeBodyTrackingMmf() {
        m_bodyState = new BodyTracking::BodyStateV2{};
    }

    void OpenXrRuntime::bodyStateWatcherThread() {
        std::vector<uint8_t> frameData(ovrvd_FrameDataSize);
        while (!m_terminateBodyStateThread) {
            if (ovrvd_GetFrameData(m_ovrSession, frameData.data(), (uint32_t)frameData.size()) == ovrSuccess) {
                BodyTracking::BodyStateV2 state{};
                FillBodyStateFromFrameData(frameData.data(), state);
                uint32_t jointCount = 0;
                static_assert(sizeof(BodyTracking::SkeletonJoint) == 36);
                ovrvd_GetBodySkeleton(m_ovrSession,
                                      reinterpret_cast<uint8_t*>(state.SkeletonJoints),
                                      (uint32_t)sizeof(state.SkeletonJoints),
                                      &jointCount,
                                      &state.SkeletonChangedCount);
                std::unique_lock lock(m_bodyStateMutex);
                m_cachedBodyState = state;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

} // namespace opendesk_openxr
