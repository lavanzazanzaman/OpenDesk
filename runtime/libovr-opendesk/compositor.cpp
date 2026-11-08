#include "compositor.h"
#include "math.h"

#include <OVR_CAPI.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#include "dxvk_interop.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#else
#include <unistd.h>
#endif

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

#ifdef _WIN32
constexpr auto opaque_memory = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr auto opaque_semaphore = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
constexpr auto opaque_memory = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr auto opaque_semaphore = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

// an exported memory / semaphore handle on its way into another device. Linux: the import takes the fd on success;
// Windows: NT handles stay ours (closed here once imported, or not).
struct moved_handle
{
	native_handle h = no_handle;
	bool owned = true;
	~moved_handle() { reset(); }
	void imported()
	{
#ifndef _WIN32
		h = no_handle; // the driver owns it now
#endif
	}
	void reset()
	{
		if (!owned || h == no_handle)
			return;
#ifdef _WIN32
		CloseHandle(h);
#else
		close(h);
#endif
		h = no_handle;
	}
};

#ifdef _WIN32
template <class T>
T * query(IUnknown * from) // QueryInterface, nullptr if not
{
	T * out = nullptr;
	if (from)
		from->QueryInterface(__uuidof(T), reinterpret_cast<void **>(&out));
	return out;
}

using memory_import_info = VkImportMemoryWin32HandleInfoKHR;
constexpr auto memory_import_type = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
void set_handle(memory_import_info & i, native_handle h) { i.handle = h; }
#else
using memory_import_info = VkImportMemoryFdInfoKHR;
constexpr auto memory_import_type = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
void set_handle(memory_import_info & i, native_handle h) { i.fd = h; }
#endif

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
#ifdef _WIN32
	return "VK_KHR_external_memory VK_KHR_external_memory_win32 VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2 "
	       "VK_KHR_external_semaphore VK_KHR_external_semaphore_win32";
#else
	return "VK_KHR_external_memory VK_KHR_external_memory_fd VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2 "
	       "VK_KHR_external_semaphore VK_KHR_external_semaphore_fd";
#endif
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
#ifdef _WIN32
	get_memory_fd = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(app_device, "vkGetMemoryWin32HandleKHR"));
	if (!get_memory_fd)
		throw std::runtime_error("the application's Vulkan device lacks VK_KHR_external_memory_win32");
#else
	get_memory_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(app_device, "vkGetMemoryFdKHR"));
	if (!get_memory_fd)
		throw std::runtime_error("the application's Vulkan device lacks VK_KHR_external_memory_fd");
#endif
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
	init_pipelines();
}

void compositor::init_pipelines()
{
	if (std::getenv("OPENDESK_OVR_LOG"))
		report("private compositor device on %p", (void *)physical);
	vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
#ifndef _WIN32
	get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
#endif

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
	if (app_physical)
		vkGetPhysicalDeviceProperties2(app_physical, &app_props);

	VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	ai.pApplicationName = "libovr-opendesk compositor";
	ai.apiVersion = VK_API_VERSION_1_2; // timeline semaphores (Windows D3D11 mode)
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
#ifdef _WIN32
		if (!app_physical) // D3D11 mode: the adapter's LUID
		{
			uint64_t l = 0;
			std::memcpy(&l, id.deviceLUID, 8);
			if (id.deviceLUIDValid && l == luid)
				physical = d;
			continue;
		}
#endif
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
#ifdef _WIN32
	std::vector<const char *> extensions{"VK_KHR_external_memory_win32"};
	const char * semaphore_extension = "VK_KHR_external_semaphore_win32";
#else
	std::vector<const char *> extensions{"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_KHR_image_format_list",
	                                     "VK_EXT_image_drm_format_modifier"};
	const char * semaphore_extension = "VK_KHR_external_semaphore_fd";
#endif
	{
		uint32_t n = 0;
		vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
		std::vector<VkExtensionProperties> available(n);
		vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, available.data());
		for (const auto & e: available)
			if (!std::strcmp(e.extensionName, semaphore_extension))
				private_semaphore_fd = true;
		if (private_semaphore_fd)
			extensions.push_back(semaphore_extension);
	}
	VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	VkPhysicalDeviceProperties dev_props{};
	vkGetPhysicalDeviceProperties(physical, &dev_props);
	const bool timeline_ok = dev_props.apiVersion >= VK_API_VERSION_1_2;
	v12.timelineSemaphore = timeline_ok;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	if (timeline_ok)
		dci.pNext = &v12;
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
	const bool app = app_vulkan(); // D3D11 mode: only our own per-slot objects (the shared fence orders the rest)
#ifdef _WIN32
	using export_fn = PFN_vkGetSemaphoreWin32HandleKHR;
	using import_fn = PFN_vkImportSemaphoreWin32HandleKHR;
	const char * export_name = "vkGetSemaphoreWin32HandleKHR";
	const char * import_name = "vkImportSemaphoreWin32HandleKHR";
#else
	using export_fn = PFN_vkGetSemaphoreFdKHR;
	using import_fn = PFN_vkImportSemaphoreFdKHR;
	const char * export_name = "vkGetSemaphoreFdKHR";
	const char * import_name = "vkImportSemaphoreFdKHR";
#endif
	auto app_get_fd = app ? reinterpret_cast<export_fn>(vkGetDeviceProcAddr(app_device, export_name)) : nullptr;
	auto app_import = app ? reinterpret_cast<import_fn>(vkGetDeviceProcAddr(app_device, import_name)) : nullptr;
	auto get_fd = reinterpret_cast<export_fn>(vkGetDeviceProcAddr(device, export_name));
	auto import = reinterpret_cast<import_fn>(vkGetDeviceProcAddr(device, import_name));
	if (app && (!private_semaphore_fd || !app_get_fd || !app_import || !get_fd || !import))
		throw std::runtime_error("the application's Vulkan device lacks external semaphores");
	// exportable on one device, imported (permanently: same payload) on the other
	auto shared = [&](VkDevice from, export_fn export_fd, VkDevice to, import_fn import_fd, VkSemaphore & a, VkSemaphore & b) {
		VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
		esci.handleTypes = opaque_semaphore;
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &esci};
		check(vkCreateSemaphore(from, &sci, nullptr, &a), "vkCreateSemaphore(exported)");
		moved_handle h;
#ifdef _WIN32
		VkSemaphoreGetWin32HandleInfoKHR gfi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
		gfi.semaphore = a;
		gfi.handleType = opaque_semaphore;
		check(export_fd(from, &gfi, &h.h), "vkGetSemaphoreWin32HandleKHR");
#else
		VkSemaphoreGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
		gfi.semaphore = a;
		gfi.handleType = opaque_semaphore;
		check(export_fd(from, &gfi, &h.h), "vkGetSemaphoreFdKHR");
#endif
		VkSemaphoreCreateInfo plain{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		check(vkCreateSemaphore(to, &plain, nullptr, &b), "vkCreateSemaphore(imported)");
#ifdef _WIN32
		VkImportSemaphoreWin32HandleInfoKHR ifi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
		ifi.handle = h.h;
#else
		VkImportSemaphoreFdInfoKHR ifi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
		ifi.fd = h.h;
#endif
		ifi.semaphore = b;
		ifi.handleType = opaque_semaphore;
		check(import_fd(to, &ifi), "import semaphore");
		h.imported();
	};
	VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256}};
	for (auto & f: frames)
	{
		VkCommandBufferAllocateInfo aai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		aai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		aai.commandBufferCount = 1;
		VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (app)
		{
			shared(app_device, app_get_fd, device, import, f.ready_app, f.ready);
			shared(device, get_fd, app_device, app_import, f.done, f.done_app);
			aai.commandPool = app_pool;
			check(vkAllocateCommandBuffers(app_device, &aai, &f.app_release), "vkAllocateCommandBuffers(app release)");
			check(vkAllocateCommandBuffers(app_device, &aai, &f.app_acquire), "vkAllocateCommandBuffers(app acquire)");
			check(vkCreateFence(app_device, &fci, nullptr, &f.app_fence), "vkCreateFence(app frame)");
		}
		aai.commandPool = pool;
		check(vkAllocateCommandBuffers(device, &aai, &f.cmd), "vkAllocateCommandBuffers(frame)");
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
		if (app_device)
		{
			vkDestroySemaphore(app_device, f.ready_app, nullptr);
			vkDestroySemaphore(app_device, f.done_app, nullptr);
			vkDestroyFence(app_device, f.app_fence, nullptr);
		}
		vkDestroySemaphore(device, f.ready, nullptr);
		vkDestroySemaphore(device, f.done, nullptr);
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
	if (app_queue)
		vkQueueWaitIdle(app_queue);
#ifdef _WIN32
	if (dxvk)
	{
		queue_idle();
		vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
	}
	else
#endif
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
#ifdef _WIN32
	vkDestroySemaphore(device, timeline, nullptr);
	if (d3d_fence)
		reinterpret_cast<IUnknown *>(d3d_fence)->Release();
	if (d3d_context)
		reinterpret_cast<IUnknown *>(d3d_context)->Release();
	if (d3d_device)
		reinterpret_cast<IUnknown *>(d3d_device)->Release();
#endif
#ifdef _WIN32
	if (dxvk) // DXVK's device and instance: not ours
		dxvk->Release();
	else
#endif
	{
		vkDestroyDevice(device, nullptr);
		vkDestroyInstance(instance, nullptr);
	}
	if (app_device)
	{
		vkDestroyFence(app_device, app_fence, nullptr);
		vkDestroyCommandPool(app_device, app_pool, nullptr);
	}
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

VkResult compositor::queue_submit(const VkSubmitInfo & si, VkFence f)
{
#ifdef _WIN32
	if (dxvk)
	{
		dxvk->FlushRenderingCommands(); // the game's D3D11 work first, then ours on the same queue
		dxvk->LockSubmissionQueue();
		VkResult r = vkQueueSubmit(queue, 1, &si, f);
		dxvk->ReleaseSubmissionQueue();
		return r;
	}
#endif
	return vkQueueSubmit(queue, 1, &si, f);
}

void compositor::queue_idle()
{
#ifdef _WIN32
	if (dxvk) // DXVK keeps submitting: wait for our own work only
	{
		std::vector<VkFence> pending;
		for (auto & f: frames)
			if (f.pending)
				pending.push_back(f.fence);
		if (!pending.empty())
			vkWaitForFences(device, uint32_t(pending.size()), pending.data(), VK_TRUE, UINT64_MAX);
		return;
	}
#endif
	vkQueueWaitIdle(queue);
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
	check(queue_submit(si, fence), "vkQueueSubmit");
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
		ext.handleTypes = opaque_memory;
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
		exp.handleTypes = opaque_memory;
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
		sc->memory_size.push_back(req.size);
		std::vector<VkImageView> views;
		if (!depth)
		{
			moved_handle fd;
#ifdef _WIN32
			VkMemoryGetWin32HandleInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
#else
			VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
#endif
			gfi.memory = mem;
			gfi.handleType = opaque_memory;
			check(get_memory_fd(app_device, &gfi, &fd.h), "export swapchain memory");
			VkImage shared;
			check(vkCreateImage(device, &ici, nullptr, &shared), "vkCreateImage(shared swapchain)");
			VkMemoryRequirements sreq;
			vkGetImageMemoryRequirements(device, shared, &sreq);
			memory_import_info imp{memory_import_type};
			imp.handleType = opaque_memory;
			set_handle(imp, fd.h);
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
				vkDestroyImage(device, shared, nullptr); // (fd: ownership only moves on success)
				check(r, "vkAllocateMemory(import swapchain)");
			}
			fd.imported();
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

#ifndef _WIN32
int compositor::export_memory(const swapchain & sc, size_t index)
{
	VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gfi.memory = sc.memory[index];
	gfi.handleType = opaque_memory;
	int fd = -1;
	check(get_memory_fd(app_device, &gfi, &fd), "export swapchain memory for OpenGL");
	return fd;
}
#endif

void compositor::destroy_swapchain(swapchain & sc)
{
	if (app_queue)
		vkQueueWaitIdle(app_queue);
	queue_idle();
#ifdef _WIN32
	for (ID3D11Texture2D * t: sc.textures)
		if (t)
			t->Release();
	sc.textures.clear();
#endif
	for (auto & vs: sc.views)
		for (VkImageView v: vs)
			vkDestroyImageView(device, v, nullptr);
#ifdef _WIN32
	if (!sc.dxvk)
#endif
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
	queue_idle();
	release_outputs();
	const auto & c = client.config();
	// how this GPU can write the XRGB8888 buffers in their layout (DRM modifier): as an R32_UINT storage image (the
	// shader packs the pixels), else only by copying into them (R32_UINT, or B8G8R8A8 which is the same bytes)
	auto features = [&](VkFormat format) -> VkFormatFeatureFlags {
#ifdef _WIN32
		(void)format;
		return 0; // no DRM modifiers: Windows streams are always opaque buffers
#else
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
#endif
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
				ext.handleTypes = opaque_memory;
				VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext};
				bci.size = img.desc.buffer_bytes;                    // identical to the streamer's buffer
				bci.usage = VkBufferUsageFlags(ipc::opaque_buffer_usage); // (vr_ipc.h: else NVIDIA faults)
				check(vkCreateBuffer(device, &bci, nullptr, &o.buffer), "vkCreateBuffer(stream buffer)");
				VkMemoryRequirements breq;
				vkGetBufferMemoryRequirements(device, o.buffer, &breq);
				moved_handle fd;
#ifdef _WIN32
				fd.h = img.fd; // the client keeps its NT handle: importing doesn't take it
				fd.owned = false;
#else
				fd.h = dup(img.fd); // Vulkan takes ownership on success
#endif
				memory_import_info imp{memory_import_type};
				imp.handleType = opaque_memory;
				set_handle(imp, fd.h);
				VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
				ded.buffer = o.buffer;
				VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
				mai.allocationSize = img.desc.size;
				// (OPAQUE_FD: the exporter's memory type)
				mai.memoryTypeIndex = memory_type(memory_properties, breq.memoryTypeBits,
				                                  img.desc.host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
				                                                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT
				                                                        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
				check(vkAllocateMemory(device, &mai, nullptr, &o.memory), "vkAllocateMemory(import stream buffer)");
				o.host_visible = img.desc.host_visible;
				fd.imported();
				vkBindBufferMemory(device, o.buffer, o.memory, 0);
				o.pitch_words = img.desc.stride / 4;
				o.uv_offset_words = img.desc.uv_offset / 4;
				o.p010 = img.desc.fourcc == ipc::fourcc_p010;
				o.full_range = img.desc.full_range;
			}
			else
			{
#ifdef _WIN32
				throw std::runtime_error("the streamer offered DMA-BUF images, which Windows has not");
#else
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
#endif
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
		if (fs->app_pending && app_vulkan())
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
	// D3D11 mode: the shared textures come from and go back to D3D11 (queue family EXTERNAL, layout GENERAL)
	const bool from_d3d = !app_vulkan();
	for (const auto & l: layers)
		for (int e = 0; e < 2; ++e)
		{
			const swapchain * sc = l.chain[e];
			if (!sc || sc->committed < 0 || sc->depth)
				continue;
			bool seen = false;
			for (const auto & b: to_read)
				seen |= b.image == sc->shared[size_t(sc->committed)] && b.subresourceRange.baseArrayLayer == l.array_index[e];
			if (seen)
				continue;
#ifdef _WIN32
			if (from_d3d && sc->dxvk)
			{
				// DXVK's image on DXVK's queue: from the layout DXVK has it in, and back (same queue: no ownership change)
				VkImage image = VK_NULL_HANDLE;
				VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
				if (auto * surface = query<IDXGIVkInteropSurface>(sc->textures[size_t(sc->committed)]))
				{
					surface->GetVulkanImageInfo(&image, &layout, nullptr);
					surface->Release();
				}
				VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
				b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l.array_index[e], 1};
				b.image = sc->shared[size_t(sc->committed)];
				b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				b.oldLayout = layout;
				b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
				b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				to_read.push_back(b);
				b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				b.newLayout = layout;
				b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
				b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				to_attachment.push_back(b);
				continue;
			}
#endif
			if (from_d3d)
			{
				VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
				b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l.array_index[e], 1};
				b.image = sc->shared[size_t(sc->committed)];
				b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
				b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
				b.dstQueueFamilyIndex = family;
				b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				to_read.push_back(b);
				b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
				b.srcQueueFamilyIndex = family;
				b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
				b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
				b.dstAccessMask = 0;
				to_attachment.push_back(b);
				continue;
			}
			VkImage app_image = sc->images[size_t(sc->committed)];
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
	uint64_t app_done = 0; // D3D11 mode: the fence value after the app's frame
#ifdef _WIN32
	if (from_d3d && dxvk)
	{
		// DXVK: the game's work and ours share a queue - queue_submit flushes the game's first
	}
	else if (from_d3d)
	{
		// the app's rendering so far, then this value: our submission waits for it on the GPU
		app_done = ++fence_value;
		d3d_context->Signal(d3d_fence, app_done);
		d3d_context->Flush();
	}
	else
#endif
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
			// layer alpha in bits 8-15 as transparency (0: fully opaque, the default)
			pc.info[1] |= int(std::lround((1.f - std::clamp(l.alpha, 0.f, 1.f)) * 255.f)) << 8;
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
			clear.effects[1] = state.settings.use_stencil ? fov_stencil_margin : 0.f; // the hidden area black
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
			VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			to_host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			const bool host = outputs[first_target].host_visible;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			                     host ? VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
			                     host ? 1 : 0, &to_host, per_slot, release, 0, nullptr);
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
#ifdef _WIN32
	if (from_d3d && dxvk)
	{
		VkFence f = fs ? fs->fence : fence;
		if (!fs)
			vkResetFences(device, 1, &fence);
		bool ok = queue_submit(si, f) == VK_SUCCESS;
		if (fs)
			fs->pending = ok;
		else if (ok)
			vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
		return ok;
	}
	if (from_d3d)
	{
		// app frame (app_done) -> composite -> composited; D3D11 waits for composited before the app renders again
		const uint64_t composited = ++fence_value;
		VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		tsi.waitSemaphoreValueCount = 1;
		tsi.pWaitSemaphoreValues = &app_done;
		tsi.signalSemaphoreValueCount = 1;
		tsi.pSignalSemaphoreValues = &composited;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		si.pNext = &tsi;
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &timeline;
		si.pWaitDstStageMask = &stage;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &timeline;
		VkFence f = fs ? fs->fence : fence;
		if (!fs)
			vkResetFences(device, 1, &fence);
		bool ok = queue_submit(si, f) == VK_SUCCESS;
		if (fs)
			fs->pending = ok;
		else if (ok)
			vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
		// even if ours failed, D3D11 must not wait for a value nobody signals
		if (ok)
			d3d_context->Wait(d3d_fence, composited);
		else
			fence_value = composited, d3d_context->Signal(d3d_fence, composited);
		return ok;
	}
#endif
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
		bool ok = queue_submit(si, fs->fence) == VK_SUCCESS;
		fs->pending = ok;
		// even if ours failed, the app's queue must not wait for a signal that never comes
		app_queue_submit(fs->app_acquire, app_acquire, ok ? fs->done_app : VK_NULL_HANDLE, VK_NULL_HANDLE, fs->app_fence);
		fs->app_pending = true;
		return ok;
	}
	vkResetFences(device, 1, &fence);
	bool ok = queue_submit(si, fence) == VK_SUCCESS;
	if (ok)
		vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
	app_submit(record_barriers, &app_acquire); // the app renders into them again
	return ok;
}

#ifdef _WIN32
// ---------------------------------------------------------------------------------------------------------------
// Direct3D 11 mode (OpenDesk XR's D3D11 core): the app renders into shared D3D11 textures; we composite them on our
// Vulkan device of the same adapter. One shared fence orders both sides on the GPU.

compositor::compositor(ID3D11Device * dev)
{
	d3d_device = query<ID3D11Device5>(dev);
	if (!d3d_device)
		throw std::runtime_error("Direct3D 11.4 (ID3D11Device5) is required");
	ID3D11DeviceContext * ctx = nullptr;
	dev->GetImmediateContext(&ctx);
	d3d_context = query<ID3D11DeviceContext4>(ctx);
	if (ctx)
		ctx->Release();
	if (!d3d_context)
		throw std::runtime_error("Direct3D 11.4 (ID3D11DeviceContext4) is required");
	// ovr_EndFrame may come from another thread than the app's rendering
	if (auto * mt = query<ID3D10Multithread>(dev))
	{
		mt->SetMultithreadProtected(TRUE);
		mt->Release();
	}
	if (auto * dxgi = query<IDXGIDevice>(dev))
	{
		IDXGIAdapter * adapter = nullptr;
		DXGI_ADAPTER_DESC desc{};
		if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
			luid = (uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
		if (adapter)
			adapter->Release();
		dxgi->Release();
	}
	// DXVK: its textures are images on its own Vulkan device - composite there, on its queue
	dev->QueryInterface(__uuidof(IDXGIVkInteropDevice1), reinterpret_cast<void **>(&dxvk));
	if (dxvk)
	{
		uint32_t queue_index = 0;
		dxvk->GetVulkanHandles(&instance, &physical, &device);
		dxvk->GetSubmissionQueue1(&queue, &queue_index, &family);
		report("DXVK device: compositing on its Vulkan device (queue family %u)", family);
		init_pipelines();
		return;
	}
	create_private_device();
	init_pipelines();
	create_d3d11_sync();
}

void compositor::create_d3d11_sync()
{
	if (!private_semaphore_fd)
		throw std::runtime_error("the GPU's Vulkan driver lacks VK_KHR_external_semaphore_win32");
	if (FAILED(d3d_device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&d3d_fence))))
		throw std::runtime_error("cannot create a shared Direct3D 11 fence");
	HANDLE h = nullptr;
	if (FAILED(d3d_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)))
		throw std::runtime_error("cannot share the Direct3D 11 fence");
	VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
	check(vkCreateSemaphore(device, &sci, nullptr, &timeline), "vkCreateSemaphore(timeline)");
	auto import = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
	VkImportSemaphoreWin32HandleInfoKHR ifi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
	ifi.semaphore = timeline;
	ifi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT; // also ID3D11Fence (D3D11_FENCE_BIT is an alias)
	ifi.handle = h;
	VkResult r = import ? import(device, &ifi) : VK_ERROR_EXTENSION_NOT_PRESENT;
	CloseHandle(h);
	check(r, "import the Direct3D 11 fence");
}

std::unique_ptr<swapchain> compositor::create_swapchain_d3d11(uint32_t dxgi_format, VkFormat format, uint32_t width, uint32_t height,
                                                              uint32_t array_size, uint32_t mip_levels, uint32_t sample_count,
                                                              uint32_t image_count, bool depth, bool cube, bool render_target, bool uav, bool shader_resource)
{
	auto sc = std::make_unique<swapchain>();
	sc->format = format;
	sc->width = width;
	sc->height = height;
	sc->layers = std::max(array_size, 1u); // cube maps: 6 (x array) like the Vulkan path
	sc->samples = std::max(sample_count, 1u);
	sc->depth = depth;
	sc->dxgi_format = dxgi_format;
	sc->mip_levels = std::max(mip_levels, 1u);
	auto get_props = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandlePropertiesKHR"));
	for (uint32_t i = 0; i < image_count; ++i)
	{
		D3D11_TEXTURE2D_DESC td{};
		td.Width = width;
		td.Height = height;
		td.MipLevels = sc->mip_levels;
		td.ArraySize = sc->layers;
		td.Format = DXGI_FORMAT(dxgi_format);
		td.SampleDesc.Count = sc->samples;
		td.Usage = D3D11_USAGE_DEFAULT;
		// color: always sampled (by us); depth: only when typeless (a typed depth format takes no shader resource view)
		td.BindFlags = (shader_resource || !depth ? D3D11_BIND_SHADER_RESOURCE : 0) |
		               (depth ? D3D11_BIND_DEPTH_STENCIL : render_target ? D3D11_BIND_RENDER_TARGET : 0) |
		               (uav && !depth ? D3D11_BIND_UNORDERED_ACCESS : 0);
		td.MiscFlags = (depth ? 0 : D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE) | (cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0);
		ID3D11Texture2D * tex = nullptr;
		if (FAILED(d3d_device->CreateTexture2D(&td, nullptr, &tex)))
			throw std::runtime_error("CreateTexture2D(swapchain) failed");
		sc->textures.push_back(tex);
		std::vector<VkImageView> views;
		if (!depth && dxvk)
		{
			// DXVK: the texture's own Vulkan image, viewed in our format when the image allows other views
			VkImage image = VK_NULL_HANDLE;
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			auto * surface = query<IDXGIVkInteropSurface>(tex);
			const HRESULT hr = surface ? surface->GetVulkanImageInfo(&image, &layout, &info) : E_NOINTERFACE;
			if (surface)
				surface->Release();
			if (FAILED(hr) || !image)
				throw std::runtime_error("DXVK did not expose the swapchain texture's Vulkan image");
			sc->shared.push_back(image);
			sc->dxvk = true;
			const VkFormat view_format = (info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) ? format : info.format;
			for (uint32_t l = 0; l < sc->layers; ++l)
			{
				VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
				vci.image = image;
				vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
				vci.format = view_format;
				vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l, 1};
				VkImageView v;
				check(vkCreateImageView(device, &vci, nullptr, &v), "vkCreateImageView(DXVK swapchain)");
				views.push_back(v);
			}
		}
		else if (!depth)
		{
			HANDLE h = nullptr;
			auto * resource = query<IDXGIResource1>(tex);
			const bool shared_ok = resource && SUCCEEDED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
			                                                                          nullptr, &h));
			if (resource)
				resource->Release();
			if (!shared_ok)
				throw std::runtime_error("cannot share a swapchain texture");
			moved_handle owned{h};
			VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
			ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
			VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
			ici.imageType = VK_IMAGE_TYPE_2D;
			ici.format = format;
			ici.extent = {width, height, 1};
			ici.mipLevels = sc->mip_levels;
			ici.arrayLayers = sc->layers;
			ici.samples = VkSampleCountFlagBits(sc->samples);
			ici.tiling = VK_IMAGE_TILING_OPTIMAL;
			ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0);
			ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImage shared;
			check(vkCreateImage(device, &ici, nullptr, &shared), "vkCreateImage(shared D3D11 texture)");
			VkMemoryRequirements req;
			vkGetImageMemoryRequirements(device, shared, &req);
			VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
			if (get_props)
				get_props(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, h, &hp);
			VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
			imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
			imp.handle = h;
			VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
			ded.image = shared;
			VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
			mai.allocationSize = req.size;
			mai.memoryTypeIndex = memory_type(memory_properties, req.memoryTypeBits & (hp.memoryTypeBits ? hp.memoryTypeBits : ~0u),
			                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VkDeviceMemory mem;
			if (VkResult r = vkAllocateMemory(device, &mai, nullptr, &mem); r != VK_SUCCESS)
			{
				vkDestroyImage(device, shared, nullptr);
				check(r, "vkAllocateMemory(import D3D11 texture)");
			}
			vkBindImageMemory(device, shared, mem, 0);
			sc->shared.push_back(shared);
			sc->shared_memory.push_back(mem);
			for (uint32_t l = 0; l < sc->layers; ++l)
			{
				VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
				vci.image = shared;
				vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
				vci.format = format;
				vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l, 1};
				VkImageView v;
				check(vkCreateImageView(device, &vci, nullptr, &v), "vkCreateImageView(D3D11 swapchain)");
				views.push_back(v);
			}
		}
		sc->views.push_back(views);
	}
	// GENERAL once, before D3D11 writes anything: every frame then acquires them from D3D11 (EXTERNAL) without
	// discarding their contents
	if (!sc->shared.empty() && !sc->dxvk)
	{
		struct ctx_t
		{
			swapchain * sc;
		} ctx{sc.get()};
		one_time_submit(
		        [](VkCommandBuffer c, void * p) {
			        auto * s = static_cast<ctx_t *>(p)->sc;
			        for (VkImage img: s->shared)
			        {
				        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
				        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
				        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				        b.image = img;
				        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
				        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
				                             nullptr, 1, &b);
			        }
		        },
		        &ctx);
	}
	return sc;
}
#endif
} // namespace od::ovr
