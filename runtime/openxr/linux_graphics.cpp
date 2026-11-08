// OpenDesk XR graphics core for Linux (Vulkan), derived from VDXR. Replaces d3d11_native.cpp / d3d12_interop.cpp / opengl_interop.cpp /
// precompositor.cpp / mirror_window.cpp and the swapchain parts that were tied to them.
//
// On Linux the only application API is Vulkan and libovr-opendesk's compositor runs on the application's own device and
// queue. So an OpenXR swapchain *is* a libovr-opendesk swapchain: the app renders straight into its VkImages, texture array
// slices are selected per layer (ovrLayerHeader::Reserved[0..1], alpha mode in Reserved[2]), and "resolving" a
// swapchain for submission is just committing the image the app released.
#include "pch.h"

#include "accessibility.h"
#include "log.h"
#include "runtime.h"
#include "utils.h"

namespace opendesk_openxr {

    using namespace opendesk_openxr::log;
    using namespace opendesk_openxr::utils;

    // Controller emulation (accessibility) is Windows-only (GameInput).
    std::unique_ptr<AccessibilityHelper> CreateAccessibilityHelper(ovrSession,
                                                                   const std::wstring&,
                                                                   const std::string&,
                                                                   const std::string&) {
        return nullptr;
    }

    namespace {
        const std::vector<VkFormat>& supportedFormats() {
            static const std::vector<VkFormat> formats = {
                // Prefer SRGB formats.
                VK_FORMAT_R8G8B8A8_SRGB,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_FORMAT_B8G8R8A8_UNORM,
                VK_FORMAT_R16G16B16A16_SFLOAT,
                // Prefer 32-bit depth.
                VK_FORMAT_D32_SFLOAT,
                VK_FORMAT_D32_SFLOAT_S8_UINT,
                VK_FORMAT_D24_UNORM_S8_UINT,
                VK_FORMAT_D16_UNORM,
            };
            return formats;
        }

        bool isSRGB(VkFormat format) {
            return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB;
        }
    } // namespace

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateSwapchainFormats
    XrResult OpenXrRuntime::xrEnumerateSwapchainFormats(XrSession session,
                                                        uint32_t formatCapacityInput,
                                                        uint32_t* formatCountOutput,
                                                        int64_t* formats) {
        if (!m_sessionCreated || session != m_sessionHandle) {
            return XR_ERROR_HANDLE_INVALID;
        }

        std::vector<int64_t> list;
        if (isLinuxOpenGLSession()) {
            list = linuxOpenGLFormats();
        } else if (!m_isHeadless) {
            for (VkFormat format : supportedFormats()) {
                list.push_back((int64_t)format);
            }
        }
        const uint32_t count = (uint32_t)list.size();
        if (formatCapacityInput && formatCapacityInput < count) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        *formatCountOutput = count;
        if (formatCapacityInput && formats) {
            std::copy(list.begin(), list.end(), formats);
        }
        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateSwapchain
    XrResult OpenXrRuntime::xrCreateSwapchain(XrSession session,
                                              const XrSwapchainCreateInfo* createInfo,
                                              XrSwapchain* swapchain) {
        if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        if (!m_sessionCreated || session != m_sessionHandle) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (m_isHeadless || !isVulkanSession()) {
            return XR_ERROR_FEATURE_UNSUPPORTED;
        }
        if (createInfo->faceCount != 1 && (!has_XR_KHR_composition_layer_cube || createInfo->faceCount != 6)) {
            return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        }
        if (createInfo->faceCount == 6 && (createInfo->arraySize != 1 || createInfo->width != createInfo->height)) {
            return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        }

        ovrTextureSwapChainDesc desc{};
        desc.Format = isLinuxOpenGLSession() ? linuxOpenGLToOvrFormat(createInfo->format)
                                             : vkToOvrTextureFormat((VkFormat)createInfo->format);
        if (desc.Format == OVR_FORMAT_UNKNOWN) {
            return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        }
        // Cube-compatible images also expose individual faces to the compositor.
        desc.Type = createInfo->faceCount == 6 ? ovrTexture_Cube : ovrTexture_2D;
        desc.StaticImage = !!(createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT);
        desc.ArraySize = createInfo->faceCount == 1 ? createInfo->arraySize : createInfo->faceCount;
        desc.Width = createInfo->width;
        desc.Height = createInfo->height;
        desc.MipLevels = createInfo->mipCount;
        desc.SampleCount = createInfo->sampleCount;
        if (desc.SampleCount > 1) Log("Resolving %u MSAA samples in the compositor\n", desc.SampleCount);

        ovrTextureSwapChain ovrSwapchain{};
        CHECK_OVRCMD(ovr_CreateTextureSwapChainVk(m_ovrSession, m_vkDevice, &desc, &ovrSwapchain));
        int length = 0;
        CHECK_OVRCMD(ovr_GetTextureSwapChainLength(m_ovrSession, ovrSwapchain, &length));
        if (desc.ArraySize > 1) {
            Log("Creating a swapchain with texture array (%d)\n", desc.ArraySize);
        }

        Swapchain& xrSwapchain = *new Swapchain;
        xrSwapchain.appSwapchain.ovrSwapchain = ovrSwapchain;
        xrSwapchain.ovrSwapchainLength = length;
        xrSwapchain.resolvedSlices.resize(desc.ArraySize);
        for (auto& slice : xrSwapchain.resolvedSlices) {
            slice.ovrSwapchain = ovrSwapchain;
        }
        xrSwapchain.ovrDesc = desc;
        xrSwapchain.xrDesc = *createInfo;
        xrSwapchain.isSRGB = desc.Format == OVR_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format == OVR_FORMAT_B8G8R8A8_UNORM_SRGB ||
                             desc.Format == OVR_FORMAT_B8G8R8X8_UNORM_SRGB;

        *swapchain = (XrSwapchain)&xrSwapchain;
        {
            std::unique_lock lock(m_swapchainsMutex);
            m_swapchains.insert(*swapchain);
        }
        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySwapchain
    XrResult OpenXrRuntime::xrDestroySwapchain(XrSwapchain swapchain) {
        std::unique_lock lock(m_swapchainsMutex);
        if (!m_swapchains.count(swapchain)) {
            return XR_ERROR_HANDLE_INVALID;
        }

        // Make sure there are no pending operations on the images.
        if (isVulkanSession()) {
            flushVulkanCommandQueue();
        }
        if (m_useAsyncSubmission && !m_needStartAsyncSubmissionThread) {
            waitForAsyncSubmissionIdle();
        }

        Swapchain& xrSwapchain = *(Swapchain*)swapchain;
        cleanupSwapchainImagesLinuxOpenGL(xrSwapchain);
        cleanupSwapchainImagesVulkan(xrSwapchain);
        if (xrSwapchain.appSwapchain.ovrSwapchain) {
            ovr_DestroyTextureSwapChain(m_ovrSession, xrSwapchain.appSwapchain.ovrSwapchain);
        }
        delete &xrSwapchain;
        m_swapchains.erase(swapchain);
        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateSwapchainImages
    XrResult OpenXrRuntime::xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                                       uint32_t imageCapacityInput,
                                                       uint32_t* imageCountOutput,
                                                       XrSwapchainImageBaseHeader* images) {
        std::unique_lock lock(m_swapchainsMutex);
        if (!m_swapchains.count(swapchain)) {
            return XR_ERROR_HANDLE_INVALID;
        }

        Swapchain& xrSwapchain = *(Swapchain*)swapchain;
        const int count = !xrSwapchain.ovrDesc.StaticImage ? xrSwapchain.ovrSwapchainLength : 1;
        if (imageCapacityInput && imageCapacityInput < (uint32_t)count) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        *imageCountOutput = count;
        if (imageCapacityInput && images && isLinuxOpenGLSession()) {
            return getSwapchainImagesLinuxOpenGL(
                xrSwapchain, reinterpret_cast<XrSwapchainImageOpenGLKHR*>(images), *imageCountOutput);
        }
        if (imageCapacityInput && images) {
            return getSwapchainImagesVulkan(
                xrSwapchain, reinterpret_cast<XrSwapchainImageVulkanKHR*>(images), *imageCountOutput);
        }
        return XR_SUCCESS;
    }

    // Commit the image the application released (once per swapchain and frame; all slices share the chain).
    void OpenXrRuntime::resolveSwapchainImage(Swapchain& xrSwapchain,
                                              uint32_t,
                                              std::set<std::pair<Swapchain*, uint32_t>>& resolved,
                                              bool) {
        if (!resolved.insert({&xrSwapchain, 0}).second) {
            return;
        }
        if (xrSwapchain.dirty && xrSwapchain.lastReleasedIndex >= 0) {
            CHECK_OVRCMD(ovrvd_CommitTextureSwapChainIndex(
                m_ovrSession, xrSwapchain.appSwapchain.ovrSwapchain, xrSwapchain.lastReleasedIndex));
            xrSwapchain.appSwapchain.lastCommittedIndex = xrSwapchain.lastReleasedIndex;
            for (auto& slice : xrSwapchain.resolvedSlices) {
                slice.lastCommittedIndex = xrSwapchain.lastReleasedIndex;
            }
            xrSwapchain.dirty = false;
        }
    }

    // No separate submission device on Linux: libovr-opendesk composites on the application's queue.
    void OpenXrRuntime::initializeSubmissionDevice(const std::string& appGraphicsApi) {
        Log("Submitting through libovr-opendesk on the application's %s queue\n", appGraphicsApi.c_str());
    }

    void OpenXrRuntime::cleanupSubmissionDevice() {
    }

    void OpenXrRuntime::flushSubmissionContext() {
    }

    void OpenXrRuntime::waitOnSubmissionDevice() {
    }

} // namespace opendesk_openxr
