#include "compositor.h"
#include "math.h"

#include <OVR_CAPI.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unistd.h>

namespace od::ovr
{
namespace
{
const uint32_t composite_spv[] =
#include "composite.spv.inc" // glslc -mfmt=c
        ;
const uint32_t composite_ms_spv[] =
#include "composite_ms.spv.inc"
        ;
const uint32_t yuv_spv[] =
#include "yuv.spv.inc"
        ;

struct yuv_constants // yuv.comp
{
	uint32_t width, height, pitch_words, uv_offset_words, p010, full_range;
};

void check(VkResult r, const char * what)
{
	if (r != VK_SUCCESS)
		throw std::runtime_error(std::string(what) + " failed (" + std::to_string(int(r)) + ")");
}

struct push_constants
{
	int32_t info[4];
	float fov_out[4];
	float rect[4];
	float a[4];
	float m0[4], m1[4], m2[4];
	float effects[4]; // gamma, sharpening; fovea target: top-left x, y in the eye render target
};
static_assert(sizeof(push_constants) <= 128); // the guaranteed push-constant size: info.w / effects.zw carry the fovea
} // namespace

const char * required_device_extensions()
{
	// external semaphores: GPU-side sync with the compositor (without them it falls back to CPU waits)
	return "VK_KHR_external_memory VK_KHR_external_memory_fd VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2 "
	       "VK_KHR_external_semaphore VK_KHR_external_semaphore_fd";
}

VkFormat vk_format_for(int f, bool & depth, bool & srgb)
{
	depth = srgb = false;
	switch (f)
	{
		case OVR_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
		case OVR_FORMAT_R8G8B8A8_UNORM_SRGB: srgb = true; return VK_FORMAT_R8G8B8A8_SRGB;
		case OVR_FORMAT_B8G8R8A8_UNORM:
		case OVR_FORMAT_B8G8R8X8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
		case OVR_FORMAT_B8G8R8A8_UNORM_SRGB:
		case OVR_FORMAT_B8G8R8X8_UNORM_SRGB: srgb = true; return VK_FORMAT_B8G8R8A8_SRGB;
		case OVR_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case OVR_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
		case OVR_FORMAT_D16_UNORM: depth = true; return VK_FORMAT_D16_UNORM;
		case OVR_FORMAT_D24_UNORM_S8_UINT: depth = true; return VK_FORMAT_D24_UNORM_S8_UINT;
		case OVR_FORMAT_D32_FLOAT: depth = true; return VK_FORMAT_D32_SFLOAT;
		case OVR_FORMAT_D32_FLOAT_S8X24_UINT: depth = true; return VK_FORMAT_D32_SFLOAT_S8_UINT;
		default: return VK_FORMAT_UNDEFINED;
	}
}

compositor::compositor(VkPhysicalDevice pd, VkDevice dev, VkQueue q, uint32_t fam) :
        app_physical(pd), app_device(dev), app_queue(q), app_family(fam)
{
	vkGetPhysicalDeviceMemoryProperties(app_physical, &app_memory_properties);
	get_memory_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(app_device, "vkGetMemoryFdKHR"));
	if (!get_memory_fd)
		throw std::runtime_error("the application's Vulkan device lacks VK_KHR_external_memory_fd");
	VkCommandPoolCreateInfo api{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	api.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	api.queueFamilyIndex = app_family;
	check(vkCreateCommandPool(app_device, &api, nullptr, &app_pool), "vkCreateCommandPool(app)");
	VkCommandBufferAllocateInfo aai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	aai.commandPool = app_pool;
	aai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	aai.commandBufferCount = 1;
	check(vkAllocateCommandBuffers(app_device, &aai, &app_cmd), "vkAllocateCommandBuffers(app)");
	VkFenceCreateInfo afi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	check(vkCreateFence(app_device, &afi, nullptr, &app_fence), "vkCreateFence(app)");

	create_private_device();
	if (std::getenv("OPENDESK_OVR_LOG"))
		report("private compositor device on %p", (void *)physical);
	vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
	get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = family;
	check(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool");
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	check(vkAllocateCommandBuffers(device, &cai, &cmd), "vkAllocateCommandBuffers");
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	check(vkCreateFence(device, &fci, nullptr, &fence), "vkCreateFence");

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.maxLod = VK_LOD_CLAMP_NONE;
	check(vkCreateSampler(device, &sci, nullptr, &sampler), "vkCreateSampler");

	VkDescriptorSetLayoutBinding bindings[2] = {
	        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	};
	VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dli.bindingCount = 2;
	dli.pBindings = bindings;
	check(vkCreateDescriptorSetLayout(device, &dli, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
	VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants)};
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &set_layout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &range;
	check(vkCreatePipelineLayout(device, &pli, nullptr, &pipeline_layout), "vkCreatePipelineLayout");
	VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = sizeof(composite_spv);
	smi.pCode = composite_spv;
	VkShaderModule module;
	check(vkCreateShaderModule(device, &smi, nullptr, &module), "vkCreateShaderModule");
	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
	cpi.layout = pipeline_layout;
	VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline);
	vkDestroyShaderModule(device, module, nullptr);
	check(r, "vkCreateComputePipelines");
	smi.codeSize = sizeof(composite_ms_spv); smi.pCode = composite_ms_spv;
	check(vkCreateShaderModule(device, &smi, nullptr, &module), "vkCreateShaderModule(MSAA)");
	cpi.stage.module = module;
	r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline_ms);
	vkDestroyShaderModule(device, module, nullptr);
	check(r, "vkCreateComputePipelines(MSAA)");
	VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256}};
	VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 256;
	dpi.poolSizeCount = 2;
	dpi.pPoolSizes = sizes;
	check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptor_pool), "vkCreateDescriptorPool");

	// 1x1 black image for passes without a layer image (the shader's clear pass still needs a valid binding)
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent = {1, 1, 1};
	ici.mipLevels = ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	check(vkCreateImage(device, &ici, nullptr, &black), "vkCreateImage(black)");
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device, black, &req);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = memory_type(memory_properties, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check(vkAllocateMemory(device, &mai, nullptr, &black_memory), "vkAllocateMemory(black)");
	vkBindImageMemory(device, black, black_memory, 0);
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = black;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8G8B8A8_UNORM;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	check(vkCreateImageView(device, &vci, nullptr, &black_view), "vkCreateImageView(black)");
	one_time_submit(
	        [](VkCommandBuffer c, void * p) {
		        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		        b.image = *static_cast<VkImage *>(p);
		        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
		                             1, &b);
	        },
	        &black);
	try
	{
		create_gpu_sync();
	}
	catch (const std::exception & e)
	{
		destroy_gpu_sync();
		report("GPU-side sync unavailable (%s): waiting on the CPU instead", e.what());
	}
}

// a device of our own on the app's GPU (matched by device UUID), with what importing the streamer's buffers needs
void compositor::create_private_device()
{
	VkPhysicalDeviceIDProperties app_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 app_props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &app_id};
	vkGetPhysicalDeviceProperties2(app_physical, &app_props);

	VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	ai.pApplicationName = "libovr-opendesk compositor";
	ai.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ii.pApplicationInfo = &ai;
	check(vkCreateInstance(&ii, nullptr, &instance), "vkCreateInstance(compositor)");
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());
	for (VkPhysicalDevice d: devices)
	{
		VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
		vkGetPhysicalDeviceProperties2(d, &props);
		if (!std::memcmp(id.deviceUUID, app_id.deviceUUID, VK_UUID_SIZE) && !std::memcmp(id.driverUUID, app_id.driverUUID, VK_UUID_SIZE))
			physical = d;
	}
	if (!physical)
		throw std::runtime_error("the application's GPU is not visible to the compositor");

	vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
	family = UINT32_MAX;
	for (uint32_t i = 0; i < count && family == UINT32_MAX; ++i)
		if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
			family = i;
	if (family == UINT32_MAX)
		throw std::runtime_error("no compute queue");
	float priority = 1;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	std::vector<const char *> extensions{"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_KHR_image_format_list",
	                                     "VK_EXT_image_drm_format_modifier"};
	{
		uint32_t n = 0;
		vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
		std::vector<VkExtensionProperties> available(n);
		vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, available.data());
		for (const auto & e: available)
			if (!std::strcmp(e.extensionName, "VK_KHR_external_semaphore_fd"))
				private_semaphore_fd = true;
		if (private_semaphore_fd)
			extensions.push_back("VK_KHR_external_semaphore_fd");
	}
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = uint32_t(extensions.size());
	dci.ppEnabledExtensionNames = extensions.data();
	check(vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice(compositor)");
	vkGetDeviceQueue(device, family, 0, &queue);
}

void compositor::create_yuv_pipeline()
{
	VkDescriptorSetLayoutBinding bindings[2] = {
	        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
	};
	VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dli.bindingCount = 2;
	dli.pBindings = bindings;
	check(vkCreateDescriptorSetLayout(device, &dli, nullptr, &yuv_set_layout), "vkCreateDescriptorSetLayout(yuv)");
	VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(yuv_constants)};
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &yuv_set_layout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &range;
	check(vkCreatePipelineLayout(device, &pli, nullptr, &yuv_pipeline_layout), "vkCreatePipelineLayout(yuv)");
	VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = sizeof(yuv_spv);
	smi.pCode = yuv_spv;
	VkShaderModule module;
	check(vkCreateShaderModule(device, &smi, nullptr, &module), "vkCreateShaderModule(yuv)");
	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
	cpi.layout = yuv_pipeline_layout;
	VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &yuv_pipeline);
	vkDestroyShaderModule(device, module, nullptr);
	check(r, "vkCreateComputePipelines(yuv)");
	VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * ipc::max_slots}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * ipc::max_slots}};
	VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 4 * ipc::max_slots;
	dpi.poolSizeCount = 2;
	dpi.pPoolSizes = sizes;
	check(vkCreateDescriptorPool(device, &dpi, nullptr, &yuv_pool), "vkCreateDescriptorPool(yuv)");
}

void compositor::create_gpu_sync()
{
	auto app_get_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(app_device, "vkGetSemaphoreFdKHR"));
	auto app_import = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(app_device, "vkImportSemaphoreFdKHR"));
	auto get_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
	auto import = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR"));
	if (!private_semaphore_fd || !app_get_fd || !app_import || !get_fd || !import)
		throw std::runtime_error("the application's Vulkan device lacks VK_KHR_external_semaphore_fd");
	// exportable on one device, imported (permanently: same payload) on the other
	auto shared = [&](VkDevice from, PFN_vkGetSemaphoreFdKHR export_fd, VkDevice to, PFN_vkImportSemaphoreFdKHR import_fd,
	                  VkSemaphore & a, VkSemaphore & b) {
		VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
		esci.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &esci};
		check(vkCreateSemaphore(from, &sci, nullptr, &a), "vkCreateSemaphore(exported)");
		VkSemaphoreGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
		gfi.semaphore = a;
		gfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		int fd = -1;
		check(export_fd(from, &gfi, &fd), "vkGetSemaphoreFdKHR");
		VkSemaphoreCreateInfo plain{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		check(vkCreateSemaphore(to, &plain, nullptr, &b), "vkCreateSemaphore(imported)");
		VkImportSemaphoreFdInfoKHR ifi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
		ifi.semaphore = b;
		ifi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		ifi.fd = fd; // owned by the driver on success
		if (VkResult r = import_fd(to, &ifi); r != VK_SUCCESS)
		{
			close(fd);
			check(r, "vkImportSemaphoreFdKHR");
		}
	};
	VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256}};
	for (auto & f: frames)
	{
		shared(app_device, app_get_fd, device, import, f.ready_app, f.ready);
		shared(device, get_fd, app_device, app_import, f.done, f.done_app);
		VkCommandBufferAllocateInfo aai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		aai.commandPool = app_pool;
		aai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		aai.commandBufferCount = 1;
		check(vkAllocateCommandBuffers(app_device, &aai, &f.app_release), "vkAllocateCommandBuffers(app release)");
		check(vkAllocateCommandBuffers(app_device, &aai, &f.app_acquire), "vkAllocateCommandBuffers(app acquire)");
		aai.commandPool = pool;
		check(vkAllocateCommandBuffers(device, &aai, &f.cmd), "vkAllocateCommandBuffers(frame)");
		VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		check(vkCreateFence(app_device, &fci, nullptr, &f.app_fence), "vkCreateFence(app frame)");
		check(vkCreateFence(device, &fci, nullptr, &f.fence), "vkCreateFence(frame)");
		VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dpi.maxSets = 256;
		dpi.poolSizeCount = 2;
		dpi.pPoolSizes = sizes;
		check(vkCreateDescriptorPool(device, &dpi, nullptr, &f.descriptors), "vkCreateDescriptorPool(frame)");
	}
	gpu_sync = true;
}

void compositor::destroy_gpu_sync()
{
	gpu_sync = false;
	for (auto & f: frames)
	{
		vkDestroySemaphore(app_device, f.ready_app, nullptr);
		vkDestroySemaphore(device, f.ready, nullptr);
		vkDestroySemaphore(device, f.done, nullptr);
		vkDestroySemaphore(app_device, f.done_app, nullptr);
		vkDestroyFence(app_device, f.app_fence, nullptr);
		vkDestroyFence(device, f.fence, nullptr);
		vkDestroyDescriptorPool(device, f.descriptors, nullptr);
		if (f.app_release)
			vkFreeCommandBuffers(app_device, app_pool, 1, &f.app_release);
		if (f.app_acquire)
			vkFreeCommandBuffers(app_device, app_pool, 1, &f.app_acquire);
		if (f.cmd)
			vkFreeCommandBuffers(device, pool, 1, &f.cmd);
		f = frame_slot{};
	}
}

bool compositor::wait_composited(uint32_t slot)
{
	if (!gpu_sync || slot >= ipc::max_slots || !frames[slot].pending)
		return true;
	return vkWaitForFences(device, 1, &frames[slot].fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
}

compositor::~compositor()
{
	vkQueueWaitIdle(app_queue);
	vkDeviceWaitIdle(device);
	destroy_gpu_sync();
	release_outputs();
	vkDestroyImageView(device, black_view, nullptr);
	vkDestroyImage(device, black, nullptr);
	vkFreeMemory(device, black_memory, nullptr);
	vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipeline(device, pipeline_ms, nullptr);
	vkDestroyPipeline(device, yuv_pipeline, nullptr);
	vkDestroyPipelineLayout(device, yuv_pipeline_layout, nullptr);
	vkDestroyDescriptorSetLayout(device, yuv_set_layout, nullptr);
	vkDestroyDescriptorPool(device, yuv_pool, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
	vkDestroySampler(device, sampler, nullptr);
	vkDestroyFence(device, fence, nullptr);
	vkDestroyCommandPool(device, pool, nullptr);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
	vkDestroyFence(app_device, app_fence, nullptr);
	vkDestroyCommandPool(app_device, app_pool, nullptr);
}

uint32_t compositor::memory_type(const VkPhysicalDeviceMemoryProperties & props, uint32_t bits, VkMemoryPropertyFlags flags)
{
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
		if (bits & (1u << i))
			return i;
	throw std::runtime_error("no suitable memory type");
}

void compositor::one_time_submit(void (*record)(VkCommandBuffer, void *), void * ctx)
{
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	record(cmd, ctx);
	vkEndCommandBuffer(cmd);
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkResetFences(device, 1, &fence);
	check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
	vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
}

void compositor::app_submit(void (*record)(VkCommandBuffer, void *), void * ctx)
{
	vkResetCommandBuffer(app_cmd, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(app_cmd, &bi);
	record(app_cmd, ctx);
	vkEndCommandBuffer(app_cmd);
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &app_cmd;
	vkResetFences(app_device, 1, &app_fence);
	check(vkQueueSubmit(app_queue, 1, &si, app_fence), "vkQueueSubmit(app)");
	vkWaitForFences(app_device, 1, &app_fence, VK_TRUE, UINT64_MAX);
}

std::unique_ptr<swapchain> compositor::create_swapchain(VkFormat format, uint32_t width, uint32_t height, uint32_t array_size,
                                                        uint32_t mip_levels, uint32_t sample_count, uint32_t image_count, bool depth, bool cube)
{
	auto sc = std::make_unique<swapchain>();
	sc->format = format;
	sc->width = width;
	sc->height = height;
	sc->layers = std::max(array_size, 1u);
	sc->samples = std::max(sample_count, 1u);
	sc->depth = depth;
	for (uint32_t i = 0; i < image_count; ++i)
	{
		// color images are shared with the compositor's device: identical parameters on both sides
		VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.pNext = depth ? nullptr : &ext;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = format;
		ici.extent = {width, height, 1};
		ici.mipLevels = std::max(mip_levels, 1u);
		ici.arrayLayers = sc->layers;
		ici.samples = VkSampleCountFlagBits(std::max(sample_count, 1u));
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		                            VK_IMAGE_USAGE_TRANSFER_DST_BIT
		                  : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		                            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0); // apps may view sRGB images as UNORM and vice versa
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImage img;
		check(vkCreateImage(app_device, &ici, nullptr, &img), "vkCreateImage(swapchain)");
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(app_device, img, &req);
		VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
		exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &exp};
		ded.image = img;
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.pNext = depth ? nullptr : &ded;
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = memory_type(app_memory_properties, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkDeviceMemory mem;
		check(vkAllocateMemory(app_device, &mai, nullptr, &mem), "vkAllocateMemory(swapchain)");
		vkBindImageMemory(app_device, img, mem, 0);
		sc->images.push_back(img);
		sc->memory.push_back(mem);
		std::vector<VkImageView> views;
		if (!depth)
		{
			VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
			gfi.memory = mem;
			gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
			int fd = -1;
			check(get_memory_fd(app_device, &gfi, &fd), "vkGetMemoryFdKHR(swapchain)");
			VkImage shared;
			check(vkCreateImage(device, &ici, nullptr, &shared), "vkCreateImage(shared swapchain)");
			VkMemoryRequirements sreq;
			vkGetImageMemoryRequirements(device, shared, &sreq);
			VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
			imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
			imp.fd = fd;
			VkMemoryDedicatedAllocateInfo sded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
			sded.image = shared;
			VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &sded};
			smai.allocationSize = sreq.size;
			smai.memoryTypeIndex = (sreq.memoryTypeBits & (1u << mai.memoryTypeIndex))
			                               ? mai.memoryTypeIndex
			                               : memory_type(memory_properties, sreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VkDeviceMemory smem;
			VkResult r = vkAllocateMemory(device, &smai, nullptr, &smem);
			if (r != VK_SUCCESS)
			{
				close(fd); // ownership only moves on success
				vkDestroyImage(device, shared, nullptr);
				check(r, "vkAllocateMemory(import swapchain)");
			}
			vkBindImageMemory(device, shared, smem, 0);
			sc->shared.push_back(shared);
			sc->shared_memory.push_back(smem);
			for (uint32_t l = 0; l < sc->layers; ++l)
			{
				VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
				vci.image = shared;
				vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
				vci.format = format;
				vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l, 1};
				VkImageView v;
				check(vkCreateImageView(device, &vci, nullptr, &v), "vkCreateImageView(swapchain)");
				views.push_back(v);
			}
		}
		sc->views.push_back(views);
	}
	// OpenXR Vulkan: acquired images are in COLOR_ATTACHMENT_OPTIMAL (depth: DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
	struct ctx_t
	{
		swapchain * sc;
	} ctx{sc.get()};
	app_submit(
	        [](VkCommandBuffer c, void * p) {
		        auto * s = static_cast<ctx_t *>(p)->sc;
		        for (VkImage img: s->images)
		        {
			        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			        b.newLayout = s->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			        b.image = img;
			        VkImageAspectFlags aspect = s->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
			        if (s->depth && (s->format == VK_FORMAT_D24_UNORM_S8_UINT || s->format == VK_FORMAT_D32_SFLOAT_S8_UINT))
				        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
			        b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
			        b.dstAccessMask = s->depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
			                             1, &b);
		        }
	        },
	        &ctx);
	return sc;
}

void compositor::destroy_swapchain(swapchain & sc)
{
	vkQueueWaitIdle(app_queue);
	vkQueueWaitIdle(queue);
	for (auto & vs: sc.views)
		for (VkImageView v: vs)
			vkDestroyImageView(device, v, nullptr);
	for (VkImage i: sc.shared)
		vkDestroyImage(device, i, nullptr);
	for (VkDeviceMemory m: sc.shared_memory)
		vkFreeMemory(device, m, nullptr);
	for (VkImage i: sc.images)
		vkDestroyImage(app_device, i, nullptr);
	for (VkDeviceMemory m: sc.memory)
		vkFreeMemory(app_device, m, nullptr);
	sc.images.clear();
	sc.memory.clear();
	sc.shared.clear();
	sc.shared_memory.clear();
	sc.views.clear();
}

void compositor::release_outputs()
{
	for (auto & o: outputs)
	{
		vkDestroyImageView(device, o.view, nullptr);
		vkDestroyImage(device, o.image, nullptr);
		vkFreeMemory(device, o.memory, nullptr);
		vkDestroyBuffer(device, o.buffer, nullptr);
		vkDestroyImageView(device, o.work_view, nullptr);
		vkDestroyImage(device, o.work, nullptr);
		vkFreeMemory(device, o.work_memory, nullptr);
	}
	if (yuv_pool)
		vkResetDescriptorPool(device, yuv_pool, 0);
	outputs.clear();
}

void compositor::import_outputs(vr_client & client)
{
	vkQueueWaitIdle(queue);
	release_outputs();
	const auto & c = client.config();
	// how this GPU can write the XRGB8888 buffers in their layout (DRM modifier): as an R32_UINT storage image (the
	// shader packs the pixels), else only by copying into them (R32_UINT, or B8G8R8A8 which is the same bytes)
	auto features = [&](VkFormat format) -> VkFormatFeatureFlags {
		VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
		VkFormatProperties2 fp{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
		vkGetPhysicalDeviceFormatProperties2(physical, format, &fp);
		std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
		list.pDrmFormatModifierProperties = mods.data();
		vkGetPhysicalDeviceFormatProperties2(physical, format, &fp);
		for (const auto & m: mods)
			if (m.drmFormatModifier == c.image.modifier)
				return m.drmFormatModifierTilingFeatures;
		return 0;
	};
	const bool opaque = c.image.memory == ipc::memory_opaque_buffer;
	if (opaque)
	{
		VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
		vkGetPhysicalDeviceProperties2(physical, &props);
		if (std::memcmp(id.deviceUUID, c.image.device_uuid, VK_UUID_SIZE) || std::memcmp(id.driverUUID, c.image.driver_uuid, VK_UUID_SIZE))
			throw std::runtime_error("the streamer's VR buffers live on another GPU or driver");
	}
	VkFormat import_format = VK_FORMAT_R32_UINT;
	bool copy = opaque;
	static const bool test_copy = [] { // harness: the copy path on any GPU
		const char * e = std::getenv("OPENDESK_TEST_COMPOSITE_COPY");
		return e && *e && *e != '0';
	}();
	if (!opaque && (test_copy || !(features(VK_FORMAT_R32_UINT) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)))
	{
		copy = true;
		if (!(features(VK_FORMAT_R32_UINT) & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) &&
		    (features(VK_FORMAT_B8G8R8A8_UNORM) & VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
			import_format = VK_FORMAT_B8G8R8A8_UNORM;
	}
	if (opaque)
		report("stream images %ux%u: composited, then converted to %s in the streamer's Vulkan memory", c.image.width, c.image.height,
		       c.image.fourcc == ipc::fourcc_p010 ? "P010" : "NV12");
	else
		report("stream images %ux%u, modifier %#llx: %s", c.image.width, c.image.height,
		             (unsigned long long)c.image.modifier,
		             !copy ? "written directly" : import_format == VK_FORMAT_R32_UINT ? "composited, then copied" : "composited, then copied (BGRA8)");
	per_slot = client.images_per_slot();
	eye_size[0] = c.eye_width;
	eye_size[1] = c.eye_height;
	periphery_size[0] = c.image.width;
	periphery_size[1] = c.image.height;
	for (uint32_t slot = 0; slot < c.slot_count; ++slot)
		for (uint32_t t = 0; t < per_slot; ++t) // left, right (+ left fovea, right fovea)
		{
			const auto & img = t < 2 ? client.eye_image(slot, int(t)) : client.fovea_image(slot, int(t - 2));
			output o;
			o.width = img.desc.width;
			o.height = img.desc.height;
			if (opaque)
			{
				VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
				ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
				VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext};
				bci.size = img.desc.buffer_bytes;                    // identical to the streamer's buffer
				bci.usage = VkBufferUsageFlags(ipc::opaque_buffer_usage); // (vr_ipc.h: else NVIDIA faults)
				check(vkCreateBuffer(device, &bci, nullptr, &o.buffer), "vkCreateBuffer(stream buffer)");
				VkMemoryRequirements breq;
				vkGetBufferMemoryRequirements(device, o.buffer, &breq);
				VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
				imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
				imp.fd = dup(img.fd); // Vulkan takes ownership on success
				VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
				ded.buffer = o.buffer;
				VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
				mai.allocationSize = img.desc.size;
				mai.memoryTypeIndex = memory_type(memory_properties, breq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
				if (VkResult r = vkAllocateMemory(device, &mai, nullptr, &o.memory); r != VK_SUCCESS)
				{
					close(imp.fd);
					check(r, "vkAllocateMemory(import stream buffer)");
				}
				vkBindBufferMemory(device, o.buffer, o.memory, 0);
				o.pitch_words = img.desc.stride / 4;
				o.uv_offset_words = img.desc.uv_offset / 4;
				o.p010 = img.desc.fourcc == ipc::fourcc_p010;
				o.full_range = img.desc.full_range;
			}
			else
			{
			VkSubresourceLayout plane{};
			plane.offset = img.desc.offset;
			plane.rowPitch = img.desc.stride;
			plane.size = 0; // must be 0 for explicit modifier layouts (VUID-VkImageDrmFormatModifierExplicitCreateInfoEXT-size-02267)
			VkImageDrmFormatModifierExplicitCreateInfoEXT mod{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
			mod.drmFormatModifier = img.desc.modifier;
			mod.drmFormatModifierPlaneCount = 1;
			mod.pPlaneLayouts = &plane;
			VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
			ext.pNext = &mod;
			ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
			VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			ici.pNext = &ext;
			ici.imageType = VK_IMAGE_TYPE_2D;
			ici.format = import_format; // XRGB8888 packed by the shader
			ici.extent = {o.width, o.height, 1};
			ici.mipLevels = 1;
			ici.arrayLayers = 1;
			ici.samples = VK_SAMPLE_COUNT_1_BIT;
			ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
			ici.usage = copy ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_STORAGE_BIT;
			ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			check(vkCreateImage(device, &ici, nullptr, &o.image), "vkCreateImage(stream image)");
			int fd = dup(img.fd); // Vulkan takes ownership on success
			VkMemoryFdPropertiesKHR fdp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
			if (get_memory_fd_properties)
				get_memory_fd_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp);
			VkMemoryRequirements req;
			vkGetImageMemoryRequirements(device, o.image, &req);
			VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
			imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
			imp.fd = fd;
			VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
			ded.pNext = &imp;
			ded.image = o.image;
			VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			mai.pNext = &ded;
			mai.allocationSize = req.size;
			mai.memoryTypeIndex = memory_type(memory_properties, req.memoryTypeBits & (fdp.memoryTypeBits ? fdp.memoryTypeBits : ~0u), 0);
			VkResult r = vkAllocateMemory(device, &mai, nullptr, &o.memory);
			if (r != VK_SUCCESS)
			{
				close(fd);
				check(r, "vkAllocateMemory(import stream image)");
			}
			vkBindImageMemory(device, o.image, o.memory, 0);
			}
			VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vci.format = VK_FORMAT_R32_UINT;
			vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			if (copy)
			{
				VkImageCreateInfo wci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
				wci.imageType = VK_IMAGE_TYPE_2D;
				wci.format = VK_FORMAT_R32_UINT;
				wci.extent = {o.width, o.height, 1};
				wci.mipLevels = wci.arrayLayers = 1;
				wci.samples = VK_SAMPLE_COUNT_1_BIT;
				wci.tiling = VK_IMAGE_TILING_OPTIMAL;
				wci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
				check(vkCreateImage(device, &wci, nullptr, &o.work), "vkCreateImage(composite image)");
				VkMemoryRequirements wreq;
				vkGetImageMemoryRequirements(device, o.work, &wreq);
				VkMemoryAllocateInfo wai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
				wai.allocationSize = wreq.size;
				wai.memoryTypeIndex = memory_type(memory_properties, wreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
				check(vkAllocateMemory(device, &wai, nullptr, &o.work_memory), "vkAllocateMemory(composite image)");
				vkBindImageMemory(device, o.work, o.work_memory, 0);
				vci.image = o.work;
				check(vkCreateImageView(device, &vci, nullptr, &o.work_view), "vkCreateImageView(composite image)");
				if (o.buffer)
				{
					if (!yuv_pipeline)
						create_yuv_pipeline();
					VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
					dai.descriptorPool = yuv_pool;
					dai.descriptorSetCount = 1;
					dai.pSetLayouts = &yuv_set_layout;
					check(vkAllocateDescriptorSets(device, &dai, &o.yuv_set), "vkAllocateDescriptorSets(yuv)");
					VkDescriptorImageInfo src{VK_NULL_HANDLE, o.work_view, VK_IMAGE_LAYOUT_GENERAL};
					VkDescriptorBufferInfo dst{o.buffer, 0, VK_WHOLE_SIZE};
					VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
					w[0].dstSet = w[1].dstSet = o.yuv_set;
					w[0].dstBinding = 0;
					w[0].descriptorCount = 1;
					w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
					w[0].pImageInfo = &src;
					w[1].dstBinding = 1;
					w[1].descriptorCount = 1;
					w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
					w[1].pBufferInfo = &dst;
					vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
				}
			}
			else
			{
				vci.image = o.image;
				check(vkCreateImageView(device, &vci, nullptr, &o.view), "vkCreateImageView(stream image)");
			}
			outputs.push_back(o);
		}
	generation = c.generation;
}

bool compositor::composite(vr_client & client, uint32_t slot, const std::vector<composite_layer> & layers, const float stream_fov[2][4],
                           const std::array<float, 4> eye_orientation[2], const std::array<float, 3> eye_position[2],
                           const std::array<float, 4> & head_orientation, const std::array<float, 3> & head_position,
                           const int32_t fovea_top_left[2][2])
{
	if (client.generation() != generation)
		import_outputs(client);
	if (per_slot * slot + per_slot - 1 >= outputs.size())
		return false;
	const uint32_t first_target = per_slot * slot;
	// GPU-side sync: this slot's own objects (its previous frame finished long ago - the streamer released it)
	frame_slot * fs = gpu_sync && slot < ipc::max_slots ? &frames[slot] : nullptr;
	if (fs)
	{
		if (fs->app_pending)
		{
			vkWaitForFences(app_device, 1, &fs->app_fence, VK_TRUE, UINT64_MAX);
			vkResetFences(app_device, 1, &fs->app_fence);
			fs->app_pending = false;
		}
		if (fs->pending)
		{
			vkWaitForFences(device, 1, &fs->fence, VK_TRUE, UINT64_MAX);
			vkResetFences(device, 1, &fs->fence);
			fs->pending = false;
		}
	}
	VkCommandBuffer cmd = fs ? fs->cmd : this->cmd;
	VkDescriptorPool descriptor_pool = fs ? fs->descriptors : this->descriptor_pool;
	// queue-ownership barriers on the app's queue, waiting/signalling the shared semaphores instead of the CPU
	auto app_queue_submit = [&](VkCommandBuffer c, std::vector<VkImageMemoryBarrier> & barriers, VkSemaphore wait, VkSemaphore signal,
	                            VkFence f) {
		vkResetCommandBuffer(c, 0);
		VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(c, &cbi);
		if (!barriers.empty())
			vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
			                     uint32_t(barriers.size()), barriers.data());
		vkEndCommandBuffer(c);
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo asi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		asi.commandBufferCount = 1;
		asi.pCommandBuffers = &c;
		asi.waitSemaphoreCount = wait ? 1 : 0;
		asi.pWaitSemaphores = &wait;
		asi.pWaitDstStageMask = &stage;
		asi.signalSemaphoreCount = signal ? 1 : 0;
		asi.pSignalSemaphores = &signal;
		check(vkQueueSubmit(app_queue, 1, &asi, f), "vkQueueSubmit(app)");
	};

	// layer images (each committed image + array slice once): the app's queue releases them (after its rendering, which
	// the wait covers), the compositor's device acquires them COLOR_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL,
	// and at the end everything goes back the same way
	std::vector<VkImageMemoryBarrier> app_release, app_acquire, to_read, to_attachment;
	for (const auto & l: layers)
		for (int e = 0; e < 2; ++e)
		{
			const swapchain * sc = l.chain[e];
			if (!sc || sc->committed < 0 || sc->depth)
				continue;
			VkImage app_image = sc->images[size_t(sc->committed)];
			bool seen = false;
			for (const auto & b: app_release)
				seen |= b.image == app_image && b.subresourceRange.baseArrayLayer == l.array_index[e];
			if (seen)
				continue;
			VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l.array_index[e], 1};
			b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			b.image = app_image;
			b.srcQueueFamilyIndex = app_family;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			app_release.push_back(b);
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			b.dstQueueFamilyIndex = app_family;
			b.srcAccessMask = 0;
			b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
			app_acquire.push_back(b);

			b.image = sc->shared[size_t(sc->committed)];
			b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			b.dstQueueFamilyIndex = family;
			b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			b.srcAccessMask = 0;
			b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			to_read.push_back(b);
			b.srcQueueFamilyIndex = family;
			b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b.dstAccessMask = 0;
			to_attachment.push_back(b);
		}
	auto record_barriers = [](VkCommandBuffer c, void * p) {
		auto * list = static_cast<std::vector<VkImageMemoryBarrier> *>(p);
		vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
		                     uint32_t(list->size()), list->data());
	};
	if (fs) // the compositor's submission waits for ready (the app's frame) on the GPU
		app_queue_submit(fs->app_release, app_release, VK_NULL_HANDLE, fs->ready_app, VK_NULL_HANDLE);
	else
		app_submit(record_barriers, &app_release); // also waits for the app's frame to finish on the GPU

	vkResetDescriptorPool(device, descriptor_pool, 0);
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	VkImageMemoryBarrier out_barriers[4];
	for (uint32_t e = 0; e < per_slot; ++e)
	{
		VkImageMemoryBarrier & b = out_barriers[e];
		b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = outputs[first_target + e].target();
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // fully rewritten every frame
		b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	}
	std::vector<VkImageMemoryBarrier> pre(to_read);
	pre.insert(pre.end(), out_barriers, out_barriers + per_slot);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
	                     uint32_t(pre.size()), pre.data());
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

	bool soft_fovea_edge = false;
	if (fovea_top_left)
	{
		ipc::shared_state now{};
		client.read_state(now);
		soft_fovea_edge = now.settings.soft_fovea_edge != 0;
	}
	auto dispatch = [&](uint32_t target, VkImageView layer_view, push_constants pc, bool msaa = false) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, msaa ? pipeline_ms : pipeline);
		const output & o = outputs[first_target + target];
		if (target >= 2 && fovea_top_left) // fovea: an unwarped crop of the eye render target
		{
			pc.info[3] = int(eye_size[0] | eye_size[1] << 16); // eye render target size (0: warped target)
			// Soft fovea edge: the periphery's size (exact in a float: width x 4096 + height) - the crop's edge
			// imitates it; 0: a hard edge like the Windows streamer
			if (pc.info[0] != 0 && pc.info[0] != 5) // layer passes (effects.x of the clear / gamma passes is their own)
				pc.effects[0] = soft_fovea_edge ? float(periphery_size[0] * 4096 + periphery_size[1]) : 0.f;
			pc.effects[2] = float(fovea_top_left[target - 2][0]);
			pc.effects[3] = float(fovea_top_left[target - 2][1]);
		}
		VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dai.descriptorPool = descriptor_pool;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &set_layout;
		VkDescriptorSet set;
		if (vkAllocateDescriptorSets(device, &dai, &set) != VK_SUCCESS)
			return;
		VkDescriptorImageInfo out_info{VK_NULL_HANDLE, o.target_view(), VK_IMAGE_LAYOUT_GENERAL};
		VkDescriptorImageInfo in_info{sampler, layer_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
		w[0].dstSet = set;
		w[0].dstBinding = 0;
		w[0].descriptorCount = 1;
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		w[0].pImageInfo = &out_info;
		w[1].dstSet = set;
		w[1].dstBinding = 1;
		w[1].descriptorCount = 1;
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[1].pImageInfo = &in_info;
		vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);
		vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		vkCmdDispatch(cmd, (o.width + 15) / 16, (o.height + 15) / 16, 1);
		VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
	};

	for (uint32_t target = 0; target < per_slot; ++target)
	{
		const int eye = int(target % 2);
		ipc::shared_state state{};
		client.read_state(state);
		push_constants clear{};
		std::memcpy(clear.fov_out, stream_fov[eye], sizeof(clear.fov_out));
		bool first = true;
		for (const auto & l: layers)
		{
			if (!(l.eye_mask & (1u << eye)))
				continue;
			const swapchain * sc = l.chain[eye];
			if (!sc || sc->committed < 0 || sc->depth)
				continue;
			push_constants pc{};
			pc.effects[1] = state.settings.sharpening;
			pc.info[0] = int(l.type) + 1;
			pc.info[2] = l.cube_face;
			pc.info[1] = (l.origin_bottom_left ? 1 : 0) | (first ? 2 : 0) | (l.premultiplied ? 4 : 0) | (l.opaque ? 8 : 0);
			std::memcpy(pc.fov_out, stream_fov[eye], sizeof(pc.fov_out));
			std::memcpy(pc.rect, l.rect[eye], sizeof(pc.rect));
			if (l.type == composite_layer::projection)
				std::memcpy(pc.a, l.fov[eye], sizeof(pc.a));
			else
			{
				// eye -> quad: R = Rq^-1 * Re, o = Rq^-1 (pe - pq); head-locked quads live in head space
				math::quat qe = eye_orientation[eye], qq = l.quad_orientation;
				math::vec3 pe = eye_position[eye], pq = l.quad_position;
				if (l.head_locked)
				{
					math::quat hq_inv = math::conjugate(head_orientation);
					pe = math::rotate(hq_inv, math::sub(pe, head_position));
					qe = math::mul(hq_inv, qe);
				}
				math::quat qi = math::conjugate(qq);
				math::mat3 r = math::to_mat3(math::mul(qi, qe));
				math::vec3 o = math::rotate(qi, math::sub(pe, pq));
				float * rows[3] = {pc.m0, pc.m1, pc.m2};
				for (int i = 0; i < 3; ++i)
				{
					rows[i][0] = r[i][0];
					rows[i][1] = r[i][1];
					rows[i][2] = r[i][2];
					rows[i][3] = o[i];
				}
				pc.a[0] = l.quad_size[0] / 2;
				pc.a[1] = l.quad_size[1] / 2;
				pc.a[2] = 1;
				if (l.type == composite_layer::cylinder) {
					pc.a[0] = l.cylinder_radius; pc.a[1] = l.cylinder_angle;
					pc.a[2] = l.cylinder_radius * l.cylinder_angle / l.cylinder_aspect;
				}
			}
			if (first && l.type != composite_layer::projection)
			{
				// a quad as the first layer: clear to black first, then blend it
				clear.info[0] = 0;
				dispatch(target, black_view, clear);
				pc.info[1] &= ~2;
			}
			dispatch(target, sc->views[size_t(sc->committed)][l.array_index[eye]], pc, sc->samples > 1);
			first = false;
		}
		if (first) // nothing submitted for this eye
		{
			clear.info[0] = 0;
			dispatch(target, black_view, clear);
		}
		// output gamma like the Windows streamer's VR shaders: linear ^ (0.9 / Gamma) - at the default Gamma 1.0 a slight
		// lift of the darks it always applies
		{
			clear.info[0] = 5;
			clear.effects[0] = 0.9f / std::max(state.settings.gamma, 0.1f);
			dispatch(target, black_view, clear);
		}
	}

	const bool copy = outputs[first_target].work != VK_NULL_HANDLE;
	if (copy) // composited image -> the streamer's DMA-BUF
	{
		const bool to_buffer = outputs[first_target].buffer != VK_NULL_HANDLE;
		VkImageMemoryBarrier to_copy[8];
		for (uint32_t e = 0; e < per_slot; ++e)
		{
			const output & o = outputs[first_target + e];
			to_copy[2 * e] = out_barriers[e];
			to_copy[2 * e].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			to_copy[2 * e].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			to_copy[2 * e].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			to_copy[2 * e].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			to_copy[2 * e + 1] = out_barriers[e];
			to_copy[2 * e + 1].image = o.image;
			to_copy[2 * e + 1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			to_copy[2 * e + 1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			to_copy[2 * e + 1].srcAccessMask = 0;
			to_copy[2 * e + 1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		}
		if (to_buffer) // NV12/P010 conversion straight into the streamer's buffers
		{
			VkMemoryBarrier written{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &written, 0, nullptr, 0,
			                     nullptr);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, yuv_pipeline);
			VkBufferMemoryBarrier release[4];
			for (uint32_t e = 0; e < per_slot; ++e)
			{
				const output & o = outputs[first_target + e];
				yuv_constants yc{o.width, o.height, o.pitch_words, o.uv_offset_words, o.p010, o.full_range};
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, yuv_pipeline_layout, 0, 1, &o.yuv_set, 0, nullptr);
				vkCmdPushConstants(cmd, yuv_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(yc), &yc);
				vkCmdDispatch(cmd, (o.width / 4 + 7) / 8, (o.height / 2 + 7) / 8, 1);
				release[e] = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
				release[e].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
				release[e].srcQueueFamilyIndex = family;
				release[e].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; // the encoder (CUDA / Vulkan) reads it next
				release[e].buffer = o.buffer;
				release[e].size = VK_WHOLE_SIZE;
			}
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, per_slot, release, 0,
			                     nullptr);
		}
		else
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2 * per_slot, to_copy);
		for (uint32_t e = 0; e < per_slot && !to_buffer; ++e)
		{
			const output & o = outputs[first_target + e];
			VkImageCopy region{};
			region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.extent = {o.width, o.height, 1};
			vkCmdCopyImage(cmd, o.work, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}
	}
	const bool image_handoff = outputs[first_target].buffer == VK_NULL_HANDLE;
	VkImageMemoryBarrier handoff[4];
	for (uint32_t e = 0; e < per_slot; ++e)
	{
		handoff[e] = out_barriers[e];
		handoff[e].image = outputs[first_target + e].image;
		handoff[e].oldLayout = copy ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
		handoff[e].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		handoff[e].srcAccessMask = copy ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT;
		handoff[e].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		handoff[e].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; // the encoder reads it next
		handoff[e].srcQueueFamilyIndex = family;
	}
	std::vector<VkImageMemoryBarrier> post(to_attachment);
	if (image_handoff)
		post.insert(post.end(), handoff, handoff + per_slot);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
	                     nullptr, 0, nullptr, uint32_t(post.size()), post.data());
	vkEndCommandBuffer(cmd);
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	if (fs)
	{
		// no CPU waits: after the app's frame (ready) -> composite -> done; the app's queue waits for done before it
		// renders into these images again; the caller waits for the fence (wait_composited) before the streamer gets it
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &fs->ready;
		si.pWaitDstStageMask = &stage;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &fs->done;
		bool ok = vkQueueSubmit(queue, 1, &si, fs->fence) == VK_SUCCESS;
		fs->pending = ok;
		// even if ours failed, the app's queue must not wait for a signal that never comes
		app_queue_submit(fs->app_acquire, app_acquire, ok ? fs->done_app : VK_NULL_HANDLE, VK_NULL_HANDLE, fs->app_fence);
		fs->app_pending = true;
		return ok;
	}
	vkResetFences(device, 1, &fence);
	bool ok = vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS;
	if (ok)
		vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
	app_submit(record_barriers, &app_acquire); // the app renders into them again
	return ok;
}
} // namespace od::ovr
