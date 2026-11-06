// What Proton's launcher shim does through xrizer before every VR game: create a Vulkan session, begin it, and end it
// again without rendering a frame. The runtime must still walk READY -> SYNCHRONIZED -> STOPPING -> IDLE -> EXITING,
// otherwise xrizer spins in xrPollEvent forever and the game waits on the shim.
//   xr-exit-test [headless]   (uses the active OpenXR runtime / XR_RUNTIME_JSON); exit code 0 on success
// headless: also enable XR_MND_headless like RiftLift's Rift bridge (OpenDesk XR then re-creates its OVR session inside
// xrCreateSession) and create a swapchain on the Vulkan session
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#define CHECK(x)                                                                                                       \
	do                                                                                                                 \
	{                                                                                                                  \
		XrResult r_ = (x);                                                                                             \
		if (XR_FAILED(r_))                                                                                             \
		{                                                                                                              \
			std::printf("FAIL  %s -> %d\n", #x, int(r_));                                                             \
			return 1;                                                                                                  \
		}                                                                                                              \
	} while (0)

static XrInstance instance;
static XrSession current_session = XR_NULL_HANDLE;
static int foreign_events = 0; // state events not carrying the current session's handle

// polls until `want` (true) or 3 s pass (false)
static bool wait_state(XrSessionState want)
{
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (std::chrono::steady_clock::now() < end)
	{
		XrEventDataBuffer e{XR_TYPE_EVENT_DATA_BUFFER};
		if (xrPollEvent(instance, &e) == XR_SUCCESS && e.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			auto & changed = reinterpret_cast<XrEventDataSessionStateChanged &>(e);
			auto state = changed.state;
			std::printf("      state %d%s\n", int(state), changed.session == current_session ? "" : " (another session's!)");
			if (changed.session != current_session)
			{
				++foreign_events;
				continue;
			}
			if (state == want)
				return true;
		}
	}
	return false;
}

int main(int argc, char ** argv)
{
	const bool headless_ext = argc > 1 && std::strcmp(argv[1], "headless") == 0;
	const char * ext[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, "XR_MND_headless"};
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(ici.applicationInfo.applicationName, "xr-exit-test");
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = headless_ext ? 2 : 1;
	ici.enabledExtensionNames = ext;
	CHECK(xrCreateInstance(&ici, &instance));
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system;
	CHECK(xrGetSystem(instance, &sgi, &system));

	{
		// XR_EPIC_view_configuration_fov: the FOV without a session
		XrViewConfigurationViewFovEPIC fov[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW_FOV_EPIC}, {XR_TYPE_VIEW_CONFIGURATION_VIEW_FOV_EPIC}};
		XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW, &fov[0]}, {XR_TYPE_VIEW_CONFIGURATION_VIEW, &fov[1]}};
		uint32_t n = 0;
		CHECK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, views));
		bool ok = fov[0].recommendedFov.angleLeft < 0 && fov[0].recommendedFov.angleRight > 0 && fov[1].recommendedFov.angleUp > 0;
		std::printf("%s  XR_EPIC_view_configuration_fov (left eye %.2f..%.2f rad)\n", ok ? "PASS" : "FAIL", fov[0].recommendedFov.angleLeft,
		            fov[0].recommendedFov.angleRight);
		if (!ok)
			return 1;
	}
	PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements;
	PFN_xrCreateVulkanInstanceKHR create_instance;
	PFN_xrGetVulkanGraphicsDevice2KHR get_device;
	PFN_xrCreateVulkanDeviceKHR create_device;
	CHECK(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&get_requirements));
	CHECK(xrGetInstanceProcAddr(instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&create_instance));
	CHECK(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&get_device));
	CHECK(xrGetInstanceProcAddr(instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&create_device));
	XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	CHECK(get_requirements(instance, system, &req));

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vici.pApplicationInfo = &app;
	XrVulkanInstanceCreateInfoKHR xvici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	xvici.systemId = system;
	xvici.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	xvici.vulkanCreateInfo = &vici;
	VkInstance vk_instance;
	VkResult vr;
	CHECK(create_instance(instance, &xvici, &vk_instance, &vr));
	XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	gdi.systemId = system;
	gdi.vulkanInstance = vk_instance;
	VkPhysicalDevice physical;
	CHECK(get_device(instance, &gdi, &physical));
	float priority = 1;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority; // family 0: graphics on every desktop GPU
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	xdci.systemId = system;
	xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	xdci.vulkanPhysicalDevice = physical;
	xdci.vulkanCreateInfo = &dci;
	VkDevice device;
	CHECK(create_device(instance, &xdci, &device, &vr));

	XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
	binding.instance = vk_instance;
	binding.physicalDevice = physical;
	binding.device = device;
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &binding;
	sci.systemId = system;
	if (headless_ext)
	{
		// RiftLift's bridge probes with headless sessions first and destroys them with events still queued
		XrSessionCreateInfo hci{XR_TYPE_SESSION_CREATE_INFO};
		hci.systemId = system;
		XrSession probe;
		CHECK(xrCreateSession(instance, &hci, &probe));
		CHECK(xrDestroySession(probe));
	}
	XrSession session;
	CHECK(xrCreateSession(instance, &sci, &session));
	current_session = session;
	if (headless_ext)
	{
		XrSwapchainCreateInfo sci2{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		sci2.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		sci2.format = VK_FORMAT_R8G8B8A8_SRGB;
		sci2.sampleCount = 1;
		sci2.width = sci2.height = 64;
		sci2.faceCount = sci2.arraySize = sci2.mipCount = 1;
		XrSwapchain chain;
		CHECK(xrCreateSwapchain(session, &sci2, &chain));
		std::printf("PASS  swapchain on a Vulkan session with XR_MND_headless enabled\n");
		xrDestroySwapchain(chain);
	}
	bool ok = wait_state(XR_SESSION_STATE_READY);
	std::printf("%s  READY\n", ok ? "PASS" : "FAIL");
	XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
	sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	CHECK(xrBeginSession(session, &sbi));
	CHECK(xrRequestExitSession(session)); // no frame rendered
	bool stopping = wait_state(XR_SESSION_STATE_STOPPING);
	std::printf("%s  exit request without a frame -> STOPPING\n", stopping ? "PASS" : "FAIL");
	if (stopping)
	{
		CHECK(xrEndSession(session));
		bool exiting = wait_state(XR_SESSION_STATE_EXITING);
		std::printf("%s  xrEndSession -> EXITING\n", exiting ? "PASS" : "FAIL");
		ok &= exiting;
	}
	ok &= stopping;
	std::printf("%s  no state events of an earlier session (%d)\n", foreign_events ? "FAIL" : "PASS", foreign_events);
	ok &= !foreign_events;
	xrDestroySession(session);
	xrDestroyInstance(instance);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(vk_instance, nullptr);
	std::printf(ok ? "ALL PASS\n" : "FAILED\n");
	return ok ? 0 : 1;
}
