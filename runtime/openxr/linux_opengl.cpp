// XR_KHR_opengl_enable on Linux (GLX, e.g. Vivecraft through xrizer).
//
// libovr-opendesk's compositor works with Vulkan swapchains on an application device. An OpenGL session is therefore a
// Vulkan session underneath, on a private device of the same GPU as the application's GL context: the swapchain memory
// (already exportable as opaque fds, the compositor shares it the same way) is imported into OpenGL through
// GL_EXT_memory_object_fd, so the application renders straight into the images that get composited.
#include "pch.h"

#include "log.h"
#include "runtime.h"
#include "utils.h"

#include <dlfcn.h>
#include <unistd.h>

namespace opendesk_openxr {

    using namespace opendesk_openxr::log;

    namespace {
        // openxr_platform.h declares this one only with X11 and GLX in scope (whose macros clash with the rest of the
        // runtime): the layout of XrGraphicsBindingOpenGLXlibKHR
        struct XlibBinding {
            XrStructureType type;
            const void* next;
            void* xDisplay;
            uint32_t visualid;
            void* glxFBConfig;
            unsigned long glxDrawable;
            void* glxContext;
        };

        constexpr unsigned GL_NO_ERROR_ = 0, GL_EXTENSIONS_ = 0x1F03, GL_NUM_EXTENSIONS_ = 0x821D, GL_TRUE_ = 1;
        constexpr unsigned GL_TEXTURE_2D_ = 0x0DE1, GL_TEXTURE_2D_ARRAY_ = 0x8C1A, GL_TEXTURE_2D_MULTISAMPLE_ = 0x9100,
                           GL_TEXTURE_2D_MULTISAMPLE_ARRAY_ = 0x9102, GL_TEXTURE_CUBE_MAP_ = 0x8513;
        constexpr unsigned GL_DEVICE_UUID_EXT_ = 0x9597, GL_DEDICATED_MEMORY_OBJECT_EXT_ = 0x9581,
                           GL_HANDLE_TYPE_OPAQUE_FD_EXT_ = 0x9586, GL_TEXTURE_TILING_EXT_ = 0x9580,
                           GL_OPTIMAL_TILING_EXT_ = 0x9584;
        constexpr unsigned GL_RGB8_ = 0x8051, GL_RGBA8_ = 0x8058, GL_SRGB8_ = 0x8C41, GL_SRGB8_ALPHA8_ = 0x8C43,
                           GL_RGBA16F_ = 0x881A, GL_R11F_G11F_B10F_ = 0x8C3A;

        // what the image memory holds (RGB formats: the Vulkan images have an alpha channel, no one reads it)
        unsigned storageFormat(unsigned format) {
            return format == GL_RGB8_ ? GL_RGBA8_ : format == GL_SRGB8_ ? GL_SRGB8_ALPHA8_ : format;
        }
    } // namespace

    struct OpenXrRuntime::LinuxOpenGL {
        // GLX
        void* (*glXGetProcAddressARB)(const unsigned char*){nullptr};
        void* (*glXGetCurrentContext)(){nullptr};
        void* (*glXGetCurrentDisplay)(){nullptr};
        unsigned long (*glXGetCurrentDrawable)(){nullptr};
        unsigned long (*glXGetCurrentReadDrawable)(){nullptr};
        int (*glXMakeContextCurrent)(void*, unsigned long, unsigned long, void*){nullptr};

        // OpenGL
        unsigned (*glGetError)(){nullptr};
        void (*glFinish)(){nullptr};
        void (*glGetIntegerv)(unsigned, int*){nullptr};
        const unsigned char* (*glGetStringi)(unsigned, unsigned){nullptr};
        void (*glGetUnsignedBytei_vEXT)(unsigned, unsigned, unsigned char*){nullptr};
        void (*glCreateTextures)(unsigned, int, unsigned*){nullptr};
        void (*glDeleteTextures)(int, const unsigned*){nullptr};
        void (*glTextureParameteri)(unsigned, unsigned, int){nullptr};
        void (*glCreateMemoryObjectsEXT)(int, unsigned*){nullptr};
        void (*glDeleteMemoryObjectsEXT)(int, const unsigned*){nullptr};
        void (*glMemoryObjectParameterivEXT)(unsigned, unsigned, const int*){nullptr};
        void (*glImportMemoryFdEXT)(unsigned, uint64_t, unsigned, int){nullptr};
        void (*glTextureStorageMem2DEXT)(unsigned, int, unsigned, int, int, unsigned, uint64_t){nullptr};
        void (*glTextureStorageMem2DMultisampleEXT)(unsigned, int, unsigned, int, int, unsigned char, unsigned, uint64_t){nullptr};
        void (*glTextureStorageMem3DEXT)(unsigned, int, unsigned, int, int, int, unsigned, uint64_t){nullptr};
        void (*glTextureStorageMem3DMultisampleEXT)(unsigned, int, unsigned, int, int, int, unsigned char, unsigned, uint64_t){nullptr};

        // the application's context (the current session)
        bool active{false};
        void* display{nullptr};
        unsigned long drawable{0};
        void* context{nullptr};

        // The private Vulkan device. Kept for the life of the process: libovr-opendesk's compositor outlives the OpenXR
        // session and still refers to it, and the next GL session (same GPU) reuses it.
        VkInstance instance{VK_NULL_HANDLE};
        VkPhysicalDevice physical{VK_NULL_HANDLE};
        VkDevice device{VK_NULL_HANDLE};
        uint32_t family{0};
        uint8_t uuid[VK_UUID_SIZE]{};

        // The application's context current on this thread for the duration (the runtime may be called without it).
        class Current {
          public:
            explicit Current(const LinuxOpenGL& gl) : m_gl(gl) {
                m_context = gl.glXGetCurrentContext();
                if (m_context == gl.context) {
                    m_ok = true;
                    return;
                }
                m_display = gl.glXGetCurrentDisplay();
                m_draw = gl.glXGetCurrentDrawable();
                m_read = gl.glXGetCurrentReadDrawable();
                m_ok = gl.glXMakeContextCurrent(gl.display, gl.drawable, gl.drawable, gl.context) != 0;
                m_switched = m_ok;
            }
            ~Current() {
                if (m_switched) {
                    if (m_context) {
                        m_gl.glXMakeContextCurrent(m_display, m_draw, m_read, m_context);
                    } else {
                        m_gl.glXMakeContextCurrent(m_gl.display, 0, 0, nullptr);
                    }
                }
            }
            bool ok() const {
                return m_ok;
            }

          private:
            const LinuxOpenGL& m_gl;
            void* m_context{nullptr};
            void* m_display{nullptr};
            unsigned long m_draw{0}, m_read{0};
            bool m_ok{false}, m_switched{false};
        };

        bool load() {
            if (glXGetProcAddressARB) {
                return true;
            }
            // already loaded by the application (possibly RTLD_LOCAL, like LWJGL does)
            void* lib = nullptr;
            for (const char* name : {"libGLX.so.0", "libGL.so.1"}) {
                if ((lib = dlopen(name, RTLD_NOW | RTLD_NOLOAD)) || (lib = dlopen(name, RTLD_NOW))) {
                    break;
                }
            }
            if (!lib) {
                return false;
            }
            const auto symbol = [&](auto& function, const char* name) {
                function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(lib, name));
                return function != nullptr;
            };
            if (!symbol(glXGetProcAddressARB, "glXGetProcAddressARB") || !symbol(glXGetCurrentContext, "glXGetCurrentContext") ||
                !symbol(glXGetCurrentDisplay, "glXGetCurrentDisplay") || !symbol(glXGetCurrentDrawable, "glXGetCurrentDrawable") ||
                !symbol(glXGetCurrentReadDrawable, "glXGetCurrentReadDrawable") ||
                !symbol(glXMakeContextCurrent, "glXMakeContextCurrent")) {
                glXGetProcAddressARB = nullptr;
                return false;
            }
            const auto proc = [&](auto& function, const char* name) {
                function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                    glXGetProcAddressARB(reinterpret_cast<const unsigned char*>(name)));
                return function != nullptr;
            };
            return proc(glGetError, "glGetError") && proc(glFinish, "glFinish") && proc(glGetIntegerv, "glGetIntegerv") &&
                   proc(glGetStringi, "glGetStringi") && proc(glGetUnsignedBytei_vEXT, "glGetUnsignedBytei_vEXT") &&
                   proc(glCreateTextures, "glCreateTextures") && proc(glDeleteTextures, "glDeleteTextures") &&
                   proc(glTextureParameteri, "glTextureParameteri") &&
                   proc(glCreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT") &&
                   proc(glDeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT") &&
                   proc(glMemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT") &&
                   proc(glImportMemoryFdEXT, "glImportMemoryFdEXT") && proc(glTextureStorageMem2DEXT, "glTextureStorageMem2DEXT") &&
                   proc(glTextureStorageMem2DMultisampleEXT, "glTextureStorageMem2DMultisampleEXT") &&
                   proc(glTextureStorageMem3DEXT, "glTextureStorageMem3DEXT") &&
                   proc(glTextureStorageMem3DMultisampleEXT, "glTextureStorageMem3DMultisampleEXT");
        }

        bool hasExtension(std::string_view name) const {
            int count = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS_, &count);
            for (int i = 0; i < count; i++) {
                if (const auto* e = glGetStringi(GL_EXTENSIONS_, unsigned(i)); e && name == reinterpret_cast<const char*>(e)) {
                    return true;
                }
            }
            return false;
        }
    };

    XrResult OpenXrRuntime::xrGetOpenGLGraphicsRequirementsKHR(XrInstance instance,
                                                               XrSystemId systemId,
                                                               XrGraphicsRequirementsOpenGLKHR* graphicsRequirements) {
        if (graphicsRequirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        if (!has_XR_KHR_opengl_enable) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        if (!m_instanceCreated || instance != (XrInstance)1) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (!m_systemCreated || systemId != (XrSystemId)1) {
            return XR_ERROR_SYSTEM_INVALID;
        }
        // (external memory objects work from OpenGL 4.2 contexts on; xrizer runs on whatever context the game has)
        graphicsRequirements->minApiVersionSupported = XR_MAKE_VERSION(3, 2, 0);
        graphicsRequirements->maxApiVersionSupported = XR_MAKE_VERSION(4, 6, 0);
        m_graphicsRequirementQueried = true;
        return XR_SUCCESS;
    }

    XrResult OpenXrRuntime::initializeLinuxOpenGL(const XrBaseInStructure& xlibBinding) {
        const auto& binding = reinterpret_cast<const XlibBinding&>(xlibBinding);
        if (!binding.xDisplay || !binding.glxContext) {
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        if (!m_linuxGl) {
            m_linuxGl = std::make_shared<LinuxOpenGL>();
        }
        LinuxOpenGL& gl = *m_linuxGl;
        if (!gl.load()) {
            Log("OpenGL: libGLX / the OpenGL functions this needs could not be loaded\n");
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        gl.display = binding.xDisplay;
        gl.drawable = binding.glxDrawable;
        gl.context = binding.glxContext;

        uint8_t uuid[VK_UUID_SIZE]{};
        {
            LinuxOpenGL::Current current(gl);
            if (!current.ok()) {
                Log("OpenGL: could not make the application's GLX context current\n");
                return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            }
            if (!gl.hasExtension("GL_EXT_memory_object") || !gl.hasExtension("GL_EXT_memory_object_fd")) {
                Log("OpenGL: this OpenGL driver lacks GL_EXT_memory_object_fd (needed to share images with the compositor)\n");
                return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            }
            gl.glGetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT_, 0, uuid);
        }

        if (gl.device == VK_NULL_HANDLE) {
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.pApplicationName = "OpenDesk XR (OpenGL session)";
            app.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            ici.pApplicationInfo = &app;
            CHECK_VKCMD(vkCreateInstance(&ici, nullptr, &gl.instance));

            uint32_t count = 0;
            vkEnumeratePhysicalDevices(gl.instance, &count, nullptr);
            std::vector<VkPhysicalDevice> devices(count);
            vkEnumeratePhysicalDevices(gl.instance, &count, devices.data());
            for (VkPhysicalDevice device : devices) {
                VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
                vkGetPhysicalDeviceProperties2(device, &properties);
                if (!memcmp(id.deviceUUID, uuid, VK_UUID_SIZE)) {
                    gl.physical = device;
                    Log("Using OpenGL on adapter: %s\n", properties.properties.deviceName);
                }
            }
            if (gl.physical == VK_NULL_HANDLE) {
                Log("OpenGL: no Vulkan device matches the application's OpenGL device\n");
                return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            }

            // Images are shared by memory, which only works on the GPU the compositor runs on.
            VkPhysicalDevice vr = VK_NULL_HANDLE;
            CHECK_OVRCMD(ovr_GetSessionPhysicalDeviceVk(
                m_ovrSession, *reinterpret_cast<ovrGraphicsLuid*>(&m_adapterLuid), gl.instance, &vr));
            if (vr != gl.physical) {
                VkPhysicalDeviceProperties a{}, b{};
                vkGetPhysicalDeviceProperties(gl.physical, &a);
                vkGetPhysicalDeviceProperties(vr, &b);
                Log("OpenGL: the application renders on %s but VR runs on %s. Start the game on the VR GPU (DRI_PRIME or "
                    "__GLX_VENDOR_LIBRARY_NAME), or select that GPU for VR.\n",
                    a.deviceName,
                    b.deviceName);
                return XR_ERROR_GRAPHICS_DEVICE_INVALID;
            }

            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gl.physical, &families, nullptr);
            std::vector<VkQueueFamilyProperties> properties(families);
            vkGetPhysicalDeviceQueueFamilyProperties(gl.physical, &families, properties.data());
            while (gl.family < families && !(properties[gl.family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                gl.family++;
            }
            const float priority = 1.f;
            VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            qci.queueFamilyIndex = gl.family;
            qci.queueCount = 1;
            qci.pQueuePriorities = &priority;

            uint32_t size = 0;
            CHECK_OVRCMD(ovr_GetDeviceExtensionsVk(*reinterpret_cast<ovrGraphicsLuid*>(&m_adapterLuid), nullptr, &size));
            std::string names(size, '\0');
            CHECK_OVRCMD(ovr_GetDeviceExtensionsVk(*reinterpret_cast<ovrGraphicsLuid*>(&m_adapterLuid), names.data(), &size));
            names.resize(strlen(names.c_str()));
            uint32_t available = 0;
            vkEnumerateDeviceExtensionProperties(gl.physical, nullptr, &available, nullptr);
            std::vector<VkExtensionProperties> supported(available);
            vkEnumerateDeviceExtensionProperties(gl.physical, nullptr, &available, supported.data());
            std::vector<const char*> extensions;
            for (const char* name : ParseExtensionString(names.data())) {
                if (std::any_of(supported.begin(), supported.end(), [&](const VkExtensionProperties& e) {
                        return std::string_view(e.extensionName) == name;
                    })) {
                    extensions.push_back(name);
                }
            }

            VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            dci.queueCreateInfoCount = 1;
            dci.pQueueCreateInfos = &qci;
            dci.enabledExtensionCount = (uint32_t)extensions.size();
            dci.ppEnabledExtensionNames = extensions.data();
            CHECK_VKCMD(vkCreateDevice(gl.physical, &dci, nullptr, &gl.device));
            memcpy(gl.uuid, uuid, VK_UUID_SIZE);
        } else if (memcmp(gl.uuid, uuid, VK_UUID_SIZE)) {
            Log("OpenGL: a later session moved to another GPU, which is not supported\n");
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }

        XrGraphicsBindingVulkanKHR vulkan{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        vulkan.instance = gl.instance;
        vulkan.physicalDevice = gl.physical;
        vulkan.device = gl.device;
        vulkan.queueFamilyIndex = gl.family;
        vulkan.queueIndex = 0;
        const XrResult result = initializeVulkan(vulkan);
        if (XR_SUCCEEDED(result)) {
            gl.active = true;
        }
        return result;
    }

    bool OpenXrRuntime::isLinuxOpenGLSession() const {
        return m_linuxGl && m_linuxGl->active;
    }

    void OpenXrRuntime::endLinuxOpenGLSession() {
        if (m_linuxGl) {
            m_linuxGl->active = false;
        }
    }

    std::vector<int64_t> OpenXrRuntime::linuxOpenGLFormats() const {
        return {GL_SRGB8_ALPHA8_, GL_RGBA8_, GL_SRGB8_, GL_RGB8_, GL_RGBA16F_, GL_R11F_G11F_B10F_};
    }

    ovrTextureFormat OpenXrRuntime::linuxOpenGLToOvrFormat(int64_t format) const {
        switch ((unsigned)format) {
        case GL_SRGB8_ALPHA8_:
        case GL_SRGB8_:
            return OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
        case GL_RGBA8_:
        case GL_RGB8_:
            return OVR_FORMAT_R8G8B8A8_UNORM;
        case GL_RGBA16F_:
            return OVR_FORMAT_R16G16B16A16_FLOAT;
        case GL_R11F_G11F_B10F_:
            return OVR_FORMAT_R11G11B10_FLOAT;
        default:
            return OVR_FORMAT_UNKNOWN; // (depth swapchains aren't shareable: their memory isn't exported)
        }
    }

    XrResult OpenXrRuntime::getSwapchainImagesLinuxOpenGL(Swapchain& xrSwapchain,
                                                          XrSwapchainImageOpenGLKHR* glImages,
                                                          uint32_t count) {
        LinuxOpenGL& gl = *m_linuxGl;
        LinuxOpenGL::Current current(gl);
        if (!current.ok()) {
            return XR_ERROR_RUNTIME_FAILURE;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (glImages[i].type != XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
        }
        if (xrSwapchain.glImages.empty()) {
            while (gl.glGetError() != GL_NO_ERROR_) {
            }
            const auto& desc = xrSwapchain.xrDesc;
            const unsigned format = storageFormat((unsigned)desc.format);
            const bool cube = desc.faceCount == 6, multisampled = desc.sampleCount > 1, array = desc.arraySize > 1;
            for (uint32_t i = 0; i < count; i++) {
                int fd = -1;
                uint64_t size = 0;
                CHECK_OVRCMD(ovrvd_ExportTextureSwapChainMemory(
                    m_ovrSession, xrSwapchain.appSwapchain.ovrSwapchain, (int)i, &fd, &size));
                unsigned memory = 0;
                gl.glCreateMemoryObjectsEXT(1, &memory);
                const int dedicated = GL_TRUE_;
                gl.glMemoryObjectParameterivEXT(memory, GL_DEDICATED_MEMORY_OBJECT_EXT_, &dedicated);
                gl.glImportMemoryFdEXT(memory, size, GL_HANDLE_TYPE_OPAQUE_FD_EXT_, fd); // (OpenGL owns the fd now)
                xrSwapchain.glMemory.push_back(memory);

                unsigned image = 0;
                const unsigned target = cube                  ? GL_TEXTURE_CUBE_MAP_
                                        : array && multisampled ? GL_TEXTURE_2D_MULTISAMPLE_ARRAY_
                                        : array                 ? GL_TEXTURE_2D_ARRAY_
                                        : multisampled          ? GL_TEXTURE_2D_MULTISAMPLE_
                                                                : GL_TEXTURE_2D_;
                gl.glCreateTextures(target, 1, &image);
                gl.glTextureParameteri(image, GL_TEXTURE_TILING_EXT_, GL_OPTIMAL_TILING_EXT_);
                if (array && multisampled) {
                    gl.glTextureStorageMem3DMultisampleEXT(
                        image, desc.sampleCount, format, desc.width, desc.height, desc.arraySize, GL_TRUE_, memory, 0);
                } else if (array) {
                    gl.glTextureStorageMem3DEXT(image, desc.mipCount, format, desc.width, desc.height, desc.arraySize, memory, 0);
                } else if (multisampled) {
                    gl.glTextureStorageMem2DMultisampleEXT(
                        image, desc.sampleCount, format, desc.width, desc.height, GL_TRUE_, memory, 0);
                } else {
                    gl.glTextureStorageMem2DEXT(image, desc.mipCount, format, desc.width, desc.height, memory, 0);
                }
                xrSwapchain.glImages.push_back(image);
            }
            if (const unsigned error = gl.glGetError(); error != GL_NO_ERROR_) {
                Log("OpenGL: importing the swapchain images failed (0x%x)\n", error);
                return XR_ERROR_RUNTIME_FAILURE;
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            glImages[i].image = xrSwapchain.glImages[i];
        }
        return XR_SUCCESS;
    }

    void OpenXrRuntime::cleanupSwapchainImagesLinuxOpenGL(Swapchain& xrSwapchain) {
        if (xrSwapchain.glImages.empty() || !m_linuxGl) {
            return;
        }
        LinuxOpenGL& gl = *m_linuxGl;
        LinuxOpenGL::Current current(gl);
        if (current.ok()) { // (at exit the application may have destroyed its context already)
            gl.glDeleteTextures((int)xrSwapchain.glImages.size(), xrSwapchain.glImages.data());
            gl.glDeleteMemoryObjectsEXT((int)xrSwapchain.glMemory.size(), xrSwapchain.glMemory.data());
        }
        xrSwapchain.glImages.clear();
        xrSwapchain.glMemory.clear();
    }

    void OpenXrRuntime::finishLinuxOpenGLFrame() {
        LinuxOpenGL::Current current(*m_linuxGl);
        if (current.ok()) {
            m_linuxGl->glFinish();
        }
    }

} // namespace opendesk_openxr
