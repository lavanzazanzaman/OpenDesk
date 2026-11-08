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

// Implements the necessary support for the XR_KHR_vulkan_enable and XR_KHR_vulkan_enable2 extensions:
// https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#XR_KHR_vulkan_enable
// https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#XR_KHR_vulkan_enable2

namespace opendesk_openxr {

    using namespace opendesk_openxr::log;
    using namespace opendesk_openxr::utils;

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetVulkanInstanceExtensionsKHR
    XrResult OpenXrRuntime::xrGetVulkanInstanceExtensionsKHR(XrInstance instance,
                                                             XrSystemId systemId,
                                                             uint32_t bufferCapacityInput,
                                                             uint32_t* bufferCountOutput,
                                                             char* buffer) {
        static const std::string_view instanceExtensions =
            "VK_KHR_external_memory_capabilities VK_KHR_external_semaphore_capabilities "
            "VK_KHR_external_fence_capabilities "
            "VK_KHR_get_physical_device_properties2";

        TraceLoggingWrite(g_traceProvider,
                          "xrGetVulkanInstanceExtensionsKHR",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"),
                          TLArg(bufferCapacityInput, "BufferCapacityInput"));

        // This function is used by our XR_KHR_vulkan_enable2 wrapper.
        if (!has_XR_KHR_vulkan_enable && !has_XR_KHR_vulkan_enable2) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        if (bufferCapacityInput && bufferCapacityInput < instanceExtensions.size()) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }

        *bufferCountOutput = (uint32_t)instanceExtensions.size() + 1;
        TraceLoggingWrite(
            g_traceProvider, "xrGetVulkanInstanceExtensionsKHR", TLArg(*bufferCountOutput, "BufferCountOutput"));

        if (bufferCapacityInput && buffer) {
            _snprintf_s(buffer, bufferCapacityInput, _TRUNCATE, "%s", instanceExtensions.data());
            TraceLoggingWrite(g_traceProvider, "xrGetVulkanInstanceExtensionsKHR", TLArg(buffer, "Extension"));
        }

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetVulkanDeviceExtensionsKHR
    XrResult OpenXrRuntime::xrGetVulkanDeviceExtensionsKHR(XrInstance instance,
                                                           XrSystemId systemId,
                                                           uint32_t bufferCapacityInput,
                                                           uint32_t* bufferCountOutput,
                                                           char* buffer) {
        // Linux: libovr-opendesk shares swapchain memory with its private compositor device as opaque fds. Nothing
        // Linux-only beyond that (no dma_buf / drm modifiers): Wine doesn't expose those to Windows games, and
        // DXVK / vkd3d-proton then fail to create their device ("make sure you have DX11").
        // External semaphores let libovr-opendesk sync with the app's queue on the GPU. Windows games never enable this
        // list themselves (under Proton a Windows device can't even enable VK_KHR_external_memory_fd - tested; wineopenxr
        // adds the runtime's extensions natively), so this carries no risk the memory extension doesn't already.
        static const std::string_view deviceExtensions =
            "VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2 VK_KHR_external_memory VK_KHR_external_memory_fd "
            "VK_KHR_external_semaphore VK_KHR_external_semaphore_fd";

        TraceLoggingWrite(g_traceProvider,
                          "xrGetVulkanDeviceExtensionsKHR",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"),
                          TLArg(bufferCapacityInput, "BufferCapacityInput"));

        // This function is used by our XR_KHR_vulkan_enable2 wrapper.
        if (!has_XR_KHR_vulkan_enable && !has_XR_KHR_vulkan_enable2) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        if (bufferCapacityInput && bufferCapacityInput < deviceExtensions.size()) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }

        *bufferCountOutput = (uint32_t)deviceExtensions.size() + 1;
        TraceLoggingWrite(
            g_traceProvider, "xrGetVulkanDeviceExtensionsKHR", TLArg(*bufferCountOutput, "BufferCountOutput"));

        if (bufferCapacityInput && buffer) {
            _snprintf_s(buffer, bufferCapacityInput, _TRUNCATE, "%s", deviceExtensions.data());
            TraceLoggingWrite(g_traceProvider, "xrGetVulkanDeviceExtensionsKHR", TLArg(buffer, "Extension"));
        }

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetVulkanGraphicsDeviceKHR
    XrResult OpenXrRuntime::xrGetVulkanGraphicsDeviceKHR(XrInstance instance,
                                                         XrSystemId systemId,
                                                         VkInstance vkInstance,
                                                         VkPhysicalDevice* vkPhysicalDevice) {
        TraceLoggingWrite(g_traceProvider,
                          "xrGetVulkanGraphicsDeviceKHR",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"),
                          TLPArg(vkInstance, "VkInstance"));

        // This function is used by our XR_KHR_vulkan_enable2 wrapper.
        if (!has_XR_KHR_vulkan_enable && !has_XR_KHR_vulkan_enable2) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        CHECK_MSG(ensureOVRSession(), "Failed to re-create OVR session\n");

        // Linux: libovr-opendesk picks the GPU the streamer encodes on (its DMA-BUFs are only importable there)
        return ovr_GetSessionPhysicalDeviceVk(
                   m_ovrSession, *reinterpret_cast<ovrGraphicsLuid*>(&m_adapterLuid), vkInstance, vkPhysicalDevice) ==
                       ovrSuccess
                   ? XR_SUCCESS
                   : XR_ERROR_RUNTIME_FAILURE;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateVulkanInstanceKHR
    // This wrapper is adapted from Khronos SDK's Vulkan plugin.
    XrResult OpenXrRuntime::xrCreateVulkanInstanceKHR(XrInstance instance,
                                                      const XrVulkanInstanceCreateInfoKHR* createInfo,
                                                      VkInstance* vulkanInstance,
                                                      VkResult* vulkanResult) {
        if (createInfo->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrCreateVulkanInstanceKHR",
                          TLXArg(instance, "Instance"),
                          TLArg((int)createInfo->systemId, "SystemId"),
                          TLArg((int)createInfo->createFlags, "CreateFlags"),
                          TLPArg(createInfo->pfnGetInstanceProcAddr, "GetInstanceProcAddr"));

        if (!has_XR_KHR_vulkan_enable2) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || createInfo->systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        uint32_t extensionNamesSize = 0;
        CHECK_XRCMD(xrGetVulkanInstanceExtensionsKHR(instance, createInfo->systemId, 0, &extensionNamesSize, nullptr));
        std::vector<char> extensionNames(extensionNamesSize);
        CHECK_XRCMD(xrGetVulkanInstanceExtensionsKHR(
            instance, createInfo->systemId, extensionNamesSize, &extensionNamesSize, &extensionNames[0]));

        // Note: This cannot outlive the extensionNames above, since it's just a collection of views into that
        // string!
        std::vector<const char*> extensions = ParseExtensionString(&extensionNames[0]);

        // Merge the runtime's request with the applications requests
        for (uint32_t i = 0; i < createInfo->vulkanCreateInfo->enabledExtensionCount; ++i) {
            extensions.push_back(createInfo->vulkanCreateInfo->ppEnabledExtensionNames[i]);
        }
        // Linux: the advertised semaphore extensions only where the GPU has them (else libovr-opendesk waits on the CPU)
        {
            auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)createInfo->pfnGetInstanceProcAddr(
                m_vkBootstrapInstance, "vkEnumerateDeviceExtensionProperties");
            uint32_t count = 0;
            std::vector<VkExtensionProperties> available;
            if (enumerate && enumerate(m_vkBootstrapPhysicalDevice, nullptr, &count, nullptr) == VK_SUCCESS) {
                available.resize(count);
                enumerate(m_vkBootstrapPhysicalDevice, nullptr, &count, available.data());
            }
            auto has = [&](const char* name) {
                return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) {
                    return std::string_view(e.extensionName) == name;
                });
            };
            if (!available.empty()) {
                std::erase_if(extensions, [&](const char* e) {
                    std::string_view n(e);
                    return (n == "VK_KHR_external_semaphore" || n == "VK_KHR_external_semaphore_fd") && !has(e);
                });
            }
        }

        for (uint32_t i = 0; i < extensions.size(); i++) {
            TraceLoggingWrite(g_traceProvider, "xrCreateVulkanInstanceKHR", TLArg(extensions[i], "Extension"));
        }

        VkInstanceCreateInfo instInfo = *createInfo->vulkanCreateInfo;
        instInfo.enabledExtensionCount = (uint32_t)extensions.size();
        instInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

        auto pfnCreateInstance = (PFN_vkCreateInstance)createInfo->pfnGetInstanceProcAddr(nullptr, "vkCreateInstance");
        *vulkanResult = pfnCreateInstance(&instInfo, createInfo->vulkanAllocator, vulkanInstance);

        TraceLoggingWrite(g_traceProvider,
                          "xrCreateVulkanInstanceKHR",
                          TLPArg(*vulkanInstance, "VkInstance"),
                          TLArg((int)*vulkanResult, "VkResult"));

        m_vkBootstrapInstance = *vulkanInstance;

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateVulkanDeviceKHR
    // This wrapper is adapted from Khronos SDK's Vulkan plugin.
    XrResult OpenXrRuntime::xrCreateVulkanDeviceKHR(XrInstance instance,
                                                    const XrVulkanDeviceCreateInfoKHR* createInfo,
                                                    VkDevice* vulkanDevice,
                                                    VkResult* vulkanResult) {
        if (createInfo->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "XrVulkanDeviceCreateInfoKHR",
                          TLXArg(instance, "Instance"),
                          TLArg((int)createInfo->systemId, "SystemId"),
                          TLArg((int)createInfo->createFlags, "CreateFlags"),
                          TLPArg(createInfo->pfnGetInstanceProcAddr, "GetInstanceProcAddr"),
                          TLPArg(createInfo->vulkanPhysicalDevice, "VkPhysicalDevice"));

        if (!has_XR_KHR_vulkan_enable2) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || createInfo->systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        uint32_t deviceExtensionNamesSize = 0;
        CHECK_XRCMD(
            xrGetVulkanDeviceExtensionsKHR(instance, createInfo->systemId, 0, &deviceExtensionNamesSize, nullptr));
        std::vector<char> deviceExtensionNames(deviceExtensionNamesSize);
        CHECK_XRCMD(xrGetVulkanDeviceExtensionsKHR(instance,
                                                   createInfo->systemId,
                                                   deviceExtensionNamesSize,
                                                   &deviceExtensionNamesSize,
                                                   &deviceExtensionNames[0]));

        // Note: This cannot outlive the extensionNames above, since it's just a collection of views into that
        // string!
        std::vector<const char*> extensions = ParseExtensionString(&deviceExtensionNames[0]);

        // Merge the runtime's request with the applications requests
        for (uint32_t i = 0; i < createInfo->vulkanCreateInfo->enabledExtensionCount; ++i) {
            extensions.push_back(createInfo->vulkanCreateInfo->ppEnabledExtensionNames[i]);
        }
        // Linux: the advertised semaphore extensions only where the GPU has them (else libovr-opendesk waits on the CPU)
        {
            auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)createInfo->pfnGetInstanceProcAddr(
                m_vkBootstrapInstance, "vkEnumerateDeviceExtensionProperties");
            uint32_t count = 0;
            std::vector<VkExtensionProperties> available;
            if (enumerate && enumerate(m_vkBootstrapPhysicalDevice, nullptr, &count, nullptr) == VK_SUCCESS) {
                available.resize(count);
                enumerate(m_vkBootstrapPhysicalDevice, nullptr, &count, available.data());
            }
            auto has = [&](const char* name) {
                return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) {
                    return std::string_view(e.extensionName) == name;
                });
            };
            if (!available.empty()) {
                std::erase_if(extensions, [&](const char* e) {
                    std::string_view n(e);
                    return (n == "VK_KHR_external_semaphore" || n == "VK_KHR_external_semaphore_fd") && !has(e);
                });
            }
        }

        for (uint32_t i = 0; i < extensions.size(); i++) {
            TraceLoggingWrite(g_traceProvider, "xrCreateVulkanDeviceKHR", TLArg(extensions[i], "Extension"));
        }

        VkDeviceCreateInfo deviceInfo = *createInfo->vulkanCreateInfo;

        // Enable timeline semaphores.
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineSemaphoreFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        timelineSemaphoreFeatures.timelineSemaphore = true;
        timelineSemaphoreFeatures.pNext = (void*)deviceInfo.pNext;

        deviceInfo.pNext = &timelineSemaphoreFeatures;
        deviceInfo.enabledExtensionCount = (uint32_t)extensions.size();
        deviceInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

        auto pfnCreateDevice =
            (PFN_vkCreateDevice)createInfo->pfnGetInstanceProcAddr(m_vkBootstrapInstance, "vkCreateDevice");
        *vulkanResult =
            pfnCreateDevice(m_vkBootstrapPhysicalDevice, &deviceInfo, createInfo->vulkanAllocator, vulkanDevice);

        TraceLoggingWrite(g_traceProvider,
                          "xrCreateVulkanDeviceKHR",
                          TLPArg(*vulkanDevice, "VkDevice"),
                          TLArg((int)*vulkanResult, "VkResult"));

        m_vkDispatch.vkGetInstanceProcAddr = createInfo->pfnGetInstanceProcAddr;
        if (createInfo->vulkanAllocator) {
            m_vkAllocator = *createInfo->vulkanAllocator;
        } else {
            m_vkAllocator.reset();
        }

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetVulkanGraphicsDevice2KHR
    // This wrapper is adapted from Khronos SDK's Vulkan plugin.
    XrResult OpenXrRuntime::xrGetVulkanGraphicsDevice2KHR(XrInstance instance,
                                                          const XrVulkanGraphicsDeviceGetInfoKHR* getInfo,
                                                          VkPhysicalDevice* vulkanPhysicalDevice) {
        if (getInfo->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrGetVulkanGraphicsDevice2KHR",
                          TLXArg(instance, "Instance"),
                          TLArg((int)getInfo->systemId, "SystemId"),
                          TLPArg(getInfo->vulkanInstance, "VkInstance"));

        if (!has_XR_KHR_vulkan_enable2) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || getInfo->systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        CHECK_XRCMD(
            xrGetVulkanGraphicsDeviceKHR(instance, getInfo->systemId, getInfo->vulkanInstance, vulkanPhysicalDevice));

        TraceLoggingWrite(
            g_traceProvider, "xrGetVulkanGraphicsDevice2KHR", TLPArg(*vulkanPhysicalDevice, "VkPhysicalDevice"));

        m_vkBootstrapPhysicalDevice = *vulkanPhysicalDevice;

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetVulkanGraphicsRequirementsKHR
    XrResult OpenXrRuntime::xrGetVulkanGraphicsRequirementsKHR(XrInstance instance,
                                                               XrSystemId systemId,
                                                               XrGraphicsRequirementsVulkanKHR* graphicsRequirements) {
        if (graphicsRequirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrGetVulkanGraphicsRequirementsKHR",
                          TLXArg(instance, "Instance"),
                          TLArg((int)systemId, "SystemId"));

        if (!has_XR_KHR_vulkan_enable && !has_XR_KHR_vulkan_enable2) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }

        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }

        if (!m_isConformanceTest) {
            graphicsRequirements->minApiVersionSupported = XR_MAKE_VERSION(1, 1, 0);
        } else {
            // Quirk: the OpenXR CTS requires Vulkan 1.0 to be advertised as minimum.
            graphicsRequirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
        }
        graphicsRequirements->maxApiVersionSupported = XR_MAKE_VERSION(2, 0, 0);

        TraceLoggingWrite(
            g_traceProvider,
            "xrGetVulkanGraphicsRequirementsKHR",
            TLArg(xr::ToString(graphicsRequirements->minApiVersionSupported).c_str(), "MinApiVersionSupported"),
            TLArg(xr::ToString(graphicsRequirements->maxApiVersionSupported).c_str(), "MaxApiVersionSupported"));

        m_graphicsRequirementQueried = true;

        return XR_SUCCESS;
    }

    // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetVulkanGraphicsRequirements2KHR
    XrResult OpenXrRuntime::xrGetVulkanGraphicsRequirements2KHR(XrInstance instance,
                                                                XrSystemId systemId,
                                                                XrGraphicsRequirementsVulkanKHR* graphicsRequirements) {
        return xrGetVulkanGraphicsRequirementsKHR(instance, systemId, graphicsRequirements);
    }

    // Initialize all the resources needed for Vulkan interoperation with the D3D11 backend.
    XrResult OpenXrRuntime::initializeVulkan(const XrGraphicsBindingVulkanKHR& vkBindings) {
        if (vkBindings.instance == VK_NULL_HANDLE || vkBindings.device == VK_NULL_HANDLE ||
            vkBindings.physicalDevice == VK_NULL_HANDLE) {
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }

        // Gather function pointers for the Vulkan device extensions we are going to use.
        initializeVulkanDispatch(vkBindings.instance);

        VkPhysicalDeviceIDProperties deviceId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &deviceId};
        m_vkDispatch.vkGetPhysicalDeviceProperties2(vkBindings.physicalDevice, &properties);
        const bool queueSupportsTimers = properties.properties.limits.timestampComputeAndGraphics;
        Log("Using Vulkan on adapter: %s\n", properties.properties.deviceName);

        // Use the instance's own handle for that GPU. Under Wine, an app's binding can carry a physical device handle
        // the Vulkan loader rejects for most calls (RiftLift's Rift bridge: "vkGetPhysicalDeviceMemoryProperties:
        // Invalid physicalDevice", which aborts the game).
        VkPhysicalDevice physicalDevice = vkBindings.physicalDevice;
        {
            uint32_t count = 0;
            m_vkDispatch.vkEnumeratePhysicalDevices(vkBindings.instance, &count, nullptr);
            std::vector<VkPhysicalDevice> devices(count);
            m_vkDispatch.vkEnumeratePhysicalDevices(vkBindings.instance, &count, devices.data());
            bool listed = false;
            VkPhysicalDevice match = VK_NULL_HANDLE;
            for (VkPhysicalDevice d : devices) {
                listed |= d == physicalDevice;
                VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
                m_vkDispatch.vkGetPhysicalDeviceProperties2(d, &props);
                if (!match && !memcmp(id.deviceUUID, deviceId.deviceUUID, VK_UUID_SIZE)) {
                    match = d;
                }
            }
            Log("Vulkan binding: instance %p, physical device %p (%s among %u of the instance's), device %p\n",
                vkBindings.instance,
                vkBindings.physicalDevice,
                listed ? "listed" : "not listed",
                count,
                vkBindings.device);
            if (!listed && match) {
                Log("The application's physical device handle is not its instance's; using the instance's handle %p\n", match);
                physicalDevice = match;
            }
        }
        m_vkInstance = vkBindings.instance;
        m_vkDevice = vkBindings.device;
        m_vkPhysicalDevice = physicalDevice;

        // Create the interop device and resources that OVR will be using.
        initializeSubmissionDevice("Vulkan");

        // Initialize common Vulkan resources.
        Log("vkGetPhysicalDeviceMemoryProperties(%p)\n", m_vkPhysicalDevice);
        m_vkDispatch.vkGetPhysicalDeviceMemoryProperties(m_vkPhysicalDevice, &m_vkMemoryProperties);
        Log("Vulkan session resources ready\n");
        m_vkDispatch.vkGetDeviceQueue(m_vkDevice, vkBindings.queueFamilyIndex, vkBindings.queueIndex, &m_vkQueue);

        // Linux: libovr-opendesk composites on the app's own queue, in submission order
        m_vkQueueFamilyIndex = vkBindings.queueFamilyIndex;
        bindVulkanToOVRSession();
        // Create an additional semaphore for host-side wait.
        {
            VkFenceCreateInfo createInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            CHECK_VKCMD(m_vkDispatch.vkCreateFence(
                m_vkDevice, &createInfo, m_vkAllocator ? &m_vkAllocator.value() : nullptr, &m_vkFenceForFlush));
        }

        // We will need command buffers to perform layout transitions.
        VkCommandPoolCreateInfo poolCreateInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCreateInfo.queueFamilyIndex = vkBindings.queueFamilyIndex;
        CHECK_VKCMD(m_vkDispatch.vkCreateCommandPool(
            m_vkDevice, &poolCreateInfo, m_vkAllocator ? &m_vkAllocator.value() : nullptr, &m_vkCmdPool));
        VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = m_vkCmdPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        CHECK_VKCMD(m_vkDispatch.vkAllocateCommandBuffers(m_vkDevice, &allocateInfo, &m_vkCmdBuffer));

        // Frame timers.
        if (queueSupportsTimers) {
            for (uint32_t i = 0; i < k_numGpuTimers; i++) {
                m_gpuTimerApp[i] = std::make_unique<VulkanGpuTimer>(m_vkDispatch,
                                                                    m_vkPhysicalDevice,
                                                                    m_vkDevice,
                                                                    m_vkQueue,
                                                                    vkBindings.queueFamilyIndex,
                                                                    m_vkAllocator);
            }
        } else {
            Log("Queue does not support timestamps. Smart Smoothing will not work properly.\n");
        }

        return XR_SUCCESS;
    }

    // Initialize the function pointers for the Vulkan instance.
    // Also after enterVisibleMode(): with XR_MND_headless enabled, xrCreateSession re-creates the OVR session after the
    // binding was handled, and the new session would know nothing of the app's Vulkan objects (RiftLift's bridge:
    // its first swapchain aborted in vkGetPhysicalDeviceMemoryProperties).
    void OpenXrRuntime::bindVulkanToOVRSession() {
        CHECK_OVRCMD(ovrvd_SetSynchronizationQueueFamilyVk(m_ovrSession, m_vkQueueFamilyIndex));
        CHECK_OVRCMD(ovrvd_SetSessionPhysicalDeviceVk(m_ovrSession, m_vkInstance, m_vkPhysicalDevice));
        CHECK_OVRCMD(ovr_SetSynchronizationQueueVk(m_ovrSession, m_vkQueue));
    }

    void OpenXrRuntime::initializeVulkanDispatch(VkInstance instance) {
        PFN_vkGetInstanceProcAddr getProcAddr =
            m_vkDispatch.vkGetInstanceProcAddr ? m_vkDispatch.vkGetInstanceProcAddr : vkGetInstanceProcAddr;

#define VK_GET_PTR(fun) m_vkDispatch.fun = reinterpret_cast<PFN_##fun>(getProcAddr(instance, #fun));

        VK_GET_PTR(vkGetPhysicalDeviceProperties2);
        VK_GET_PTR(vkGetPhysicalDeviceMemoryProperties);
        VK_GET_PTR(vkEnumeratePhysicalDevices);
        VK_GET_PTR(vkGetImageMemoryRequirements2KHR);
        VK_GET_PTR(vkGetDeviceQueue);
        VK_GET_PTR(vkQueueSubmit);
        VK_GET_PTR(vkCreateImage);
        VK_GET_PTR(vkDestroyImage);
        VK_GET_PTR(vkAllocateMemory);
        VK_GET_PTR(vkFreeMemory);
        VK_GET_PTR(vkCreateCommandPool);
        VK_GET_PTR(vkDestroyCommandPool);
        VK_GET_PTR(vkAllocateCommandBuffers);
        VK_GET_PTR(vkFreeCommandBuffers);
        VK_GET_PTR(vkResetCommandBuffer);
        VK_GET_PTR(vkBeginCommandBuffer);
        VK_GET_PTR(vkCmdPipelineBarrier);
        VK_GET_PTR(vkCmdResetQueryPool);
        VK_GET_PTR(vkCmdWriteTimestamp);
        VK_GET_PTR(vkEndCommandBuffer);
        VK_GET_PTR(vkGetMemoryFdPropertiesKHR);
        VK_GET_PTR(vkBindImageMemory);
        VK_GET_PTR(vkCreateSemaphore);
        VK_GET_PTR(vkDestroySemaphore);
        VK_GET_PTR(vkImportSemaphoreFdKHR);
        VK_GET_PTR(vkCreateFence);
        VK_GET_PTR(vkDestroyFence);
        VK_GET_PTR(vkResetFences);
        VK_GET_PTR(vkWaitForFences);
        VK_GET_PTR(vkDeviceWaitIdle);
        VK_GET_PTR(vkCreateQueryPool);
        VK_GET_PTR(vkDestroyQueryPool);
        VK_GET_PTR(vkGetQueryPoolResults);

#undef VK_GET_PTR
    }

    void OpenXrRuntime::cleanupVulkan() {
        if (m_vkDispatch.vkDeviceWaitIdle) {
            m_vkDispatch.vkDeviceWaitIdle(m_vkDevice);
        }

        for (uint32_t i = 0; i < k_numGpuTimers; i++) {
            m_gpuTimerApp[i].reset();
        }
        if (m_vkDispatch.vkDestroySemaphore) {
            m_vkDispatch.vkDestroySemaphore(
                m_vkDevice, m_vkTimelineSemaphore, m_vkAllocator ? &m_vkAllocator.value() : nullptr);
            m_vkTimelineSemaphore = VK_NULL_HANDLE;
            m_vkDispatch.vkDestroyFence(
                m_vkDevice, m_vkFenceForFlush, m_vkAllocator ? &m_vkAllocator.value() : nullptr);
            m_vkFenceForFlush = VK_NULL_HANDLE;
        }
        if (m_vkDispatch.vkResetCommandBuffer) {
            m_vkDispatch.vkResetCommandBuffer(m_vkCmdBuffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
        }
        if (m_vkDispatch.vkFreeCommandBuffers) {
            m_vkDispatch.vkFreeCommandBuffers(m_vkDevice, m_vkCmdPool, 1, &m_vkCmdBuffer);
            m_vkCmdBuffer = VK_NULL_HANDLE;
        }
        if (m_vkDispatch.vkDestroyCommandPool) {
            m_vkDispatch.vkDestroyCommandPool(
                m_vkDevice, m_vkCmdPool, m_vkAllocator ? &m_vkAllocator.value() : nullptr);
            m_vkCmdPool = VK_NULL_HANDLE;
        }

        // The runtime does not own any of these. Just clear the handles.
        m_vkBootstrapInstance = VK_NULL_HANDLE;
        m_vkBootstrapPhysicalDevice = VK_NULL_HANDLE;
        m_vkInstance = VK_NULL_HANDLE;
        m_vkDevice = VK_NULL_HANDLE;
        m_vkDispatch = {};
        m_vkAllocator.reset();
        m_vkPhysicalDevice = VK_NULL_HANDLE;
        m_vkQueue = VK_NULL_HANDLE;
    }

    bool OpenXrRuntime::isVulkanSession() const {
        return m_vkDevice != VK_NULL_HANDLE;
    }

    // Retrieve the swapchain images (VkImage) for the application to use.
    // Linux: the images are libovr-opendesk's, created on the app's device - hand them out as they are
    XrResult OpenXrRuntime::getSwapchainImagesVulkan(Swapchain& xrSwapchain,
                                                     XrSwapchainImageVulkanKHR* vkImages,
                                                     uint32_t count) {
        if (vkImages[0].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        xrSwapchain.vkImages.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            CHECK_OVRCMD(ovr_GetTextureSwapChainBufferVk(
                m_ovrSession, xrSwapchain.appSwapchain.ovrSwapchain, (int)i, &xrSwapchain.vkImages[i]));
            vkImages[i].image = xrSwapchain.vkImages[i];
        }
        return XR_SUCCESS;
    }

    void OpenXrRuntime::cleanupSwapchainImagesVulkan(Swapchain& xrSwapchain) {
        xrSwapchain.vkImages.clear(); // owned by libovr-opendesk
    }

    // Wait for all pending commands to finish.
    void OpenXrRuntime::flushVulkanCommandQueue() {
        if (m_vkDispatch.vkQueueSubmit) {
            TraceLoggingWrite(g_traceProvider, "FlushContext_Wait", TLArg("Vulkan", "Api"));
            CHECK_VKCMD(m_vkDispatch.vkResetFences(m_vkDevice, 1, &m_vkFenceForFlush));
            VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            CHECK_VKCMD(m_vkDispatch.vkQueueSubmit(m_vkQueue, 1, &submitInfo, m_vkFenceForFlush));
            CHECK_VKCMD(m_vkDispatch.vkWaitForFences(m_vkDevice, 1, &m_vkFenceForFlush, true, 1'000'000'000));
        }
    }

    // Serialize commands from the Vulkan queue to the D3D11 context used by OVR.
    void OpenXrRuntime::serializeVulkanFrame() {
    }

} // namespace opendesk_openxr