// An XR_EXTX_overlay app in miniature: one magenta quad in front of the viewer, as WayVR would submit it.
//   xr-overlay-test [seconds [alpha]]
// Prints the runtime's session states and main-session visibility events; the harness looks for the quad in the stream.
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

#define VK(x)                                                                                                          \
	do                                                                                                                 \
	{                                                                                                                  \
		VkResult r_ = (x);                                                                                             \
		if (r_ != VK_SUCCESS)                                                                                          \
		{                                                                                                              \
			std::printf("FAIL  %s -> %d\n", #x, int(r_));                                                             \
			return 1;                                                                                                  \
		}                                                                                                              \
	} while (0)

int main(int argc, char ** argv)
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
	const float alpha = argc > 2 ? float(std::atof(argv[2])) : 1.f; // premultiplied magenta with this alpha
	const char * ext[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_EXTX_OVERLAY_EXTENSION_NAME};
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(ici.applicationInfo.applicationName, "xr-overlay-test");
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = ext;
	XrInstance instance;
	CHECK(xrCreateInstance(&ici, &instance));
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system;
	CHECK(xrGetSystem(instance, &sgi, &system));

	PFN_xrGetVulkanGraphicsRequirements2KHR requirements;
	PFN_xrCreateVulkanInstanceKHR create_instance;
	PFN_xrGetVulkanGraphicsDevice2KHR graphics_device;
	PFN_xrCreateVulkanDeviceKHR create_device;
	CHECK(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&requirements));
	CHECK(xrGetInstanceProcAddr(instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&create_instance));
	CHECK(xrGetInstanceProcAddr(instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&graphics_device));
	CHECK(xrGetInstanceProcAddr(instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&create_device));
	XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	CHECK(requirements(instance, system, &req));

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vici.pApplicationInfo = &app;
	XrVulkanInstanceCreateInfoKHR xvi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	xvi.systemId = system;
	xvi.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	xvi.vulkanCreateInfo = &vici;
	VkInstance vk;
	VkResult vr;
	CHECK(create_instance(instance, &xvi, &vk, &vr));
	VK(vr);
	XrVulkanGraphicsDeviceGetInfoKHR xdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	xdi.systemId = system;
	xdi.vulkanInstance = vk;
	VkPhysicalDevice physical;
	CHECK(graphics_device(instance, &xdi, &physical));
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
	uint32_t family = 0;
	while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		++family;
	const float priority = 1;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	XrVulkanDeviceCreateInfoKHR xdc{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	xdc.systemId = system;
	xdc.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	xdc.vulkanPhysicalDevice = physical;
	xdc.vulkanCreateInfo = &dci;
	VkDevice device;
	CHECK(create_device(instance, &xdc, &device, &vr));
	VK(vr);
	VkQueue queue;
	vkGetDeviceQueue(device, family, 0, &queue);
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = family;
	VkCommandPool pool;
	VK(vkCreateCommandPool(device, &pci, nullptr, &pool));
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkCommandBuffer cmd;
	VK(vkAllocateCommandBuffers(device, &cai, &cmd));

	// the overlay goes after the graphics binding, as WayVR chains it
	XrSessionCreateInfoOverlayEXTX overlay{XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX};
	overlay.sessionLayersPlacement = 5;
	XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
	binding.next = &overlay;
	binding.instance = vk;
	binding.physicalDevice = physical;
	binding.device = device;
	binding.queueFamilyIndex = family;
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &binding;
	sci.systemId = system;
	XrSession session;
	CHECK(xrCreateSession(instance, &sci, &session));
	std::printf("ok    overlay session created\n");

	XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	rsi.poseInReferenceSpace.orientation.w = 1;
	XrSpace view;
	CHECK(xrCreateReferenceSpace(session, &rsi, &view));

	uint32_t format_count = 0;
	CHECK(xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr));
	std::vector<int64_t> formats(format_count);
	CHECK(xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data()));
	XrSwapchainCreateInfo swi{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swi.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swi.format = VK_FORMAT_R8G8B8A8_SRGB;
	for (int64_t f: formats)
		if (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB)
		{
			swi.format = f;
			break;
		}
	swi.sampleCount = 1;
	swi.width = swi.height = 256;
	swi.faceCount = 1;
	swi.arraySize = 1;
	swi.mipCount = 1;
	XrSwapchain swapchain;
	CHECK(xrCreateSwapchain(session, &swi, &swapchain));
	uint32_t image_count = 0;
	CHECK(xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr));
	std::vector<XrSwapchainImageVulkan2KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
	CHECK(xrEnumerateSwapchainImages(swapchain, image_count, &image_count, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())));

	bool running = false;
	bool visible = false;
	int rendered = 0;
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (std::chrono::steady_clock::now() < end)
	{
		XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(instance, &ev) == XR_SUCCESS)
		{
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			{
				auto * st = reinterpret_cast<XrEventDataSessionStateChanged *>(&ev);
				std::printf("      session state %d\n", int(st->state));
				if (st->state == XR_SESSION_STATE_READY)
				{
					XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					CHECK(xrBeginSession(session, &bi));
					running = true;
				}
				else if (st->state == XR_SESSION_STATE_STOPPING)
				{
					CHECK(xrEndSession(session));
					running = false;
				}
			}
			else if (ev.type == XR_TYPE_EVENT_DATA_MAIN_SESSION_VISIBILITY_CHANGED_EXTX)
			{
				visible = reinterpret_cast<XrEventDataMainSessionVisibilityChangedEXTX *>(&ev)->visible;
				std::printf("ok    main session visible: %d\n", int(visible));
			}
			ev = {XR_TYPE_EVENT_DATA_BUFFER};
		}
		if (!running)
		{
			struct timespec ts{0, 10000000};
			nanosleep(&ts, nullptr);
			continue;
		}
		XrFrameState frame{XR_TYPE_FRAME_STATE};
		CHECK(xrWaitFrame(session, nullptr, &frame));
		CHECK(xrBeginFrame(session, nullptr));
		XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
		const XrCompositionLayerBaseHeader * layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader *>(&quad)};
		if (frame.shouldRender)
		{
			uint32_t index;
			CHECK(xrAcquireSwapchainImage(swapchain, nullptr, &index));
			XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = XR_INFINITE_DURATION;
			CHECK(xrWaitSwapchainImage(swapchain, &wi));
			VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			VK(vkResetCommandBuffer(cmd, 0));
			VK(vkBeginCommandBuffer(cmd, &cbi));
			VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b.image = images[index].image;
			b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			VkClearColorValue magenta{};
			magenta.float32[0] = magenta.float32[2] = magenta.float32[3] = alpha;
			vkCmdClearColorImage(cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &b.subresourceRange);
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			VK(vkEndCommandBuffer(cmd));
			VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cmd;
			VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
			VK(vkQueueWaitIdle(queue));
			CHECK(xrReleaseSwapchainImage(swapchain, nullptr));
			quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			quad.space = view;
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain = swapchain;
			quad.subImage.imageRect.extent = {256, 256};
			quad.pose.orientation.w = 1;
			quad.pose.position.z = -1;
			quad.size = {0.5f, 0.5f};
			++rendered;
		}
		XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
		fei.displayTime = frame.predictedDisplayTime;
		fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fei.layerCount = frame.shouldRender ? 1 : 0;
		fei.layers = layers;
		CHECK(xrEndFrame(session, &fei));
	}
	std::printf("ok    rendered %d frames (a game was %s at the end)\n", rendered, visible ? "showing" : "not showing");
	xrDestroyInstance(instance);
	return 0;
}
