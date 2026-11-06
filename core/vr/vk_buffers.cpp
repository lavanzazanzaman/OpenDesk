#include "vk_buffers.h"
#include "vr_ipc.h"

#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include "win_gpu.h"
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#else
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#endif

namespace od
{
#ifdef _WIN32
uint64_t dmabuf_image::next_id() // dmabuf.cpp on Linux
{
	static std::atomic<uint64_t> id{1};
	return id++;
}
#endif

namespace
{
#ifdef _WIN32
constexpr auto external_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr const char * external_memory_extension = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
#else
constexpr auto external_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr const char * external_memory_extension = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
#endif

void check(VkResult r, const char * what)
{
	if (r != VK_SUCCESS)
		throw std::runtime_error(std::format("{} failed ({})", what, int(r)));
}

uint32_t memory_type(const VkPhysicalDeviceMemoryProperties & props, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
		if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
			return i;
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
		if (bits & (1u << i))
			return i;
	throw std::runtime_error("no Vulkan memory type for the VR buffers");
}

// an instance + device on the GPU behind a DRM render node (Windows: a DXGI adapter LUID), with external memory support
struct gpu
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkPhysicalDeviceMemoryProperties memory{};
	std::array<uint8_t, 16> device_uuid{}, driver_uuid{};
#ifdef _WIN32
	PFN_vkGetMemoryWin32HandleKHR get_handle = nullptr;
#else
	PFN_vkGetMemoryFdKHR get_fd = nullptr;
#endif

	explicit gpu(const std::string & node)
	{
#ifdef _WIN32
		const auto want_luid = win::luid_of(node);
#else
		struct stat st{};
		if (stat(node.c_str(), &st) != 0)
			throw std::runtime_error("cannot stat " + node);
#endif
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &app;
		check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
		uint32_t n = 0;
		vkEnumeratePhysicalDevices(instance, &n, nullptr);
		std::vector<VkPhysicalDevice> devices(n);
		vkEnumeratePhysicalDevices(instance, &n, devices.data());
		for (VkPhysicalDevice d: devices)
		{
#ifdef _WIN32
			VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
#else
			VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
			VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &drm};
#endif
			VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
			vkGetPhysicalDeviceProperties2(d, &props);
#ifdef _WIN32
			uint64_t luid = 0;
			std::memcpy(&luid, id.deviceLUID, 8); // LowPart, then HighPart: HighPart << 32 | LowPart, like win_gpu
			const bool match = want_luid ? (id.deviceLUIDValid && luid == *want_luid)
			                             : props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || devices.size() == 1;
			if (match)
#else
			if (drm.hasRender && uint32_t(drm.renderMajor) == major(st.st_rdev) && uint32_t(drm.renderMinor) == minor(st.st_rdev))
#endif
			{
				physical = d;
				std::memcpy(device_uuid.data(), id.deviceUUID, 16);
				std::memcpy(driver_uuid.data(), id.driverUUID, 16);
				break;
			}
		}
		if (!physical)
		{
			vkDestroyInstance(instance, nullptr);
			throw std::runtime_error("no Vulkan device for " + node);
		}
		vkGetPhysicalDeviceMemoryProperties(physical, &memory);
		uint32_t count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
		for (uint32_t i = 0; i < count; ++i)
			if (families[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
			{
				family = i;
				break;
			}
		float priority = 1;
		VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qci.queueFamilyIndex = family;
		qci.queueCount = 1;
		qci.pQueuePriorities = &priority;
		const char * ext[] = {external_memory_extension};
		VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
#ifdef _WIN32
		// NV12/P010 images (the encoder's D3D11 frame pool, opaque_buffer_copier): multi-planar formats
		VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
		VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &ycbcr};
		vkGetPhysicalDeviceFeatures2(physical, &supported);
		if (ycbcr.samplerYcbcrConversion)
			dci.pNext = &ycbcr;
#endif
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = 1;
		dci.ppEnabledExtensionNames = ext;
		if (VkResult r = vkCreateDevice(physical, &dci, nullptr, &device); r != VK_SUCCESS)
		{
			vkDestroyInstance(instance, nullptr);
			check(r, "vkCreateDevice");
		}
		vkGetDeviceQueue(device, family, 0, &queue);
#ifdef _WIN32
		get_handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
#else
		get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
#endif
	}
	~gpu()
	{
		vkDestroyDevice(device, nullptr);
		vkDestroyInstance(instance, nullptr);
	}
};

VkBuffer create_buffer(VkDevice device, uint64_t size, VkBufferUsageFlags usage)
{
	VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
	ext.handleTypes = external_handle_type;
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext};
	bci.size = size;
	bci.usage = usage;
	VkBuffer buffer;
	check(vkCreateBuffer(device, &bci, nullptr, &buffer), "vkCreateBuffer(VR buffer)");
	return buffer;
}
} // namespace

struct vk_buffer_pool::impl : gpu
{
	using gpu::gpu;
};

vk_buffer_pool::vk_buffer_pool(const std::string & node) :
        p(std::make_shared<impl>(node))
{
#ifdef _WIN32
	if (!p->get_handle)
		throw std::runtime_error("vkGetMemoryWin32HandleKHR unavailable");
#else
	if (!p->get_fd)
		throw std::runtime_error("vkGetMemoryFdKHR unavailable");
#endif
}

vk_buffer_pool::~vk_buffer_pool() = default;

const std::array<uint8_t, 16> & vk_buffer_pool::device_uuid() const
{
	return p->device_uuid;
}

const std::array<uint8_t, 16> & vk_buffer_pool::driver_uuid() const
{
	return p->driver_uuid;
}

std::unique_ptr<dmabuf_image, void (*)(dmabuf_image *)> vk_buffer_pool::allocate(uint32_t w, uint32_t h, bool ten_bit)
{
	auto deleter = [](dmabuf_image * i) {
#ifdef _WIN32
		if (i->fd)
			CloseHandle(i->fd);
#else
		if (i->fd >= 0)
			close(i->fd);
#endif
		if (i->release)
			i->release();
		delete i;
	};
	std::unique_ptr<dmabuf_image, void (*)(dmabuf_image *)> img(new dmabuf_image, deleter);
	img->width = w;
	img->height = h;
	img->stride = (w * (ten_bit ? 2 : 1) + 255) & ~255u; // CUDA/NVENC pitch alignment
	img->fourcc = ten_bit ? ipc::fourcc_p010 : ipc::fourcc_nv12;
	img->memory = ipc::memory_opaque_buffer;
	img->uv_offset = img->stride * h;
	img->buffer_bytes = uint64_t(img->stride) * h * 3 / 2;
	VkBuffer buffer = create_buffer(p->device, img->buffer_bytes, ipc::opaque_buffer_usage);
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(p->device, buffer, &req);
	VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	exp.handleTypes = external_handle_type;
	VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &exp};
	ded.buffer = buffer;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = memory_type(p->memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory memory;
	if (VkResult r = vkAllocateMemory(p->device, &mai, nullptr, &memory); r != VK_SUCCESS)
	{
		vkDestroyBuffer(p->device, buffer, nullptr);
		check(r, "vkAllocateMemory(VR buffer)");
	}
	vkBindBufferMemory(p->device, buffer, memory, 0);
#ifdef _WIN32
	VkMemoryGetWin32HandleInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
	gfi.memory = memory;
	gfi.handleType = external_handle_type;
	HANDLE fd = nullptr;
	VkResult r = p->get_handle(p->device, &gfi, &fd);
#else
	VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gfi.memory = memory;
	gfi.handleType = external_handle_type;
	int fd = -1;
	VkResult r = p->get_fd(p->device, &gfi, &fd);
#endif
	std::shared_ptr<impl> owner = p;
	img->release = [owner, buffer, memory] {
		vkDestroyBuffer(owner->device, buffer, nullptr);
		vkFreeMemory(owner->device, memory, nullptr);
	};
	check(r, "export VR buffer memory");
	img->fd = fd;
	img->size = req.size;
	return img;
}

struct opaque_buffer_reader::impl : gpu
{
	using gpu::gpu;
	struct imported
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint64_t size = 0;
	};
	std::map<uint64_t, imported> buffers; // by dmabuf_image::id
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_memory = VK_NULL_HANDLE;
	uint64_t staging_size = 0;
	void * mapped = nullptr;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;

	void drop(imported & b)
	{
		vkDestroyBuffer(device, b.buffer, nullptr);
		vkFreeMemory(device, b.memory, nullptr);
	}
	~impl()
	{
		vkDeviceWaitIdle(device);
		for (auto & [id, b]: buffers)
			drop(b);
		if (staging_memory)
			vkFreeMemory(device, staging_memory, nullptr);
		vkDestroyBuffer(device, staging, nullptr);
		vkDestroyFence(device, fence, nullptr);
		vkDestroyCommandPool(device, pool, nullptr);
	}
};

opaque_buffer_reader::opaque_buffer_reader(const std::string & node) :
        p(std::make_unique<impl>(node))
{
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = p->family;
	check(vkCreateCommandPool(p->device, &pci, nullptr, &p->pool), "vkCreateCommandPool");
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = p->pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	check(vkAllocateCommandBuffers(p->device, &cai, &p->cmd), "vkAllocateCommandBuffers");
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	check(vkCreateFence(p->device, &fci, nullptr, &p->fence), "vkCreateFence");
}

opaque_buffer_reader::~opaque_buffer_reader() = default;

const uint8_t * opaque_buffer_reader::read(native_handle fd, uint64_t size, uint64_t buffer_bytes, uint64_t buffer_id)
{
	auto it = p->buffers.find(buffer_id);
	if (it == p->buffers.end() && p->buffers.size() >= 12) // the pool was replaced: forget the old imports
	{
		vkDeviceWaitIdle(p->device);
		for (auto & [id, b]: p->buffers)
			p->drop(b);
		p->buffers.clear();
	}
	if (it == p->buffers.end())
	{
		impl::imported b;
		b.size = size;
		b.buffer = create_buffer(p->device, buffer_bytes, ipc::opaque_buffer_usage); // identical to the exporter's
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(p->device, b.buffer, &req);
#ifdef _WIN32
		VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
		imp.handleType = external_handle_type;
		imp.handle = fd; // NT handles stay ours: importing doesn't take ownership
#else
		VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
		imp.handleType = external_handle_type;
		imp.fd = dup(fd); // Vulkan owns it on success
#endif
		VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
		ded.buffer = b.buffer;
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
		mai.allocationSize = size;
		mai.memoryTypeIndex = memory_type(p->memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (VkResult r = vkAllocateMemory(p->device, &mai, nullptr, &b.memory); r != VK_SUCCESS)
		{
#ifndef _WIN32
			close(imp.fd);
#endif
			vkDestroyBuffer(p->device, b.buffer, nullptr);
			check(r, "vkAllocateMemory(import VR buffer)");
		}
		vkBindBufferMemory(p->device, b.buffer, b.memory, 0);
		it = p->buffers.emplace(buffer_id, b).first;
	}
	if (p->staging_size < buffer_bytes)
	{
		if (p->staging_memory)
		{
			vkFreeMemory(p->device, p->staging_memory, nullptr);
			vkDestroyBuffer(p->device, p->staging, nullptr);
		}
		VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bci.size = buffer_bytes;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		check(vkCreateBuffer(p->device, &bci, nullptr, &p->staging), "vkCreateBuffer(staging)");
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(p->device, p->staging, &req);
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = memory_type(p->memory, req.memoryTypeBits,
		                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
		check(vkAllocateMemory(p->device, &mai, nullptr, &p->staging_memory), "vkAllocateMemory(staging)");
		vkBindBufferMemory(p->device, p->staging, p->staging_memory, 0);
		check(vkMapMemory(p->device, p->staging_memory, 0, VK_WHOLE_SIZE, 0, &p->mapped), "vkMapMemory(staging)");
		p->staging_size = buffer_bytes;
	}
	vkResetCommandBuffer(p->cmd, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(p->cmd, &bi);
	VkBufferMemoryBarrier acquire{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
	acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	acquire.dstQueueFamilyIndex = p->family;
	acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	acquire.buffer = it->second.buffer;
	acquire.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(p->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &acquire, 0, nullptr);
	VkBufferCopy region{0, 0, buffer_bytes};
	vkCmdCopyBuffer(p->cmd, it->second.buffer, p->staging, 1, &region);
	VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(p->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
	vkEndCommandBuffer(p->cmd);
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &p->cmd;
	vkResetFences(p->device, 1, &p->fence);
	check(vkQueueSubmit(p->queue, 1, &si, p->fence), "vkQueueSubmit(readback)");
	vkWaitForFences(p->device, 1, &p->fence, VK_TRUE, UINT64_MAX);
	return static_cast<const uint8_t *>(p->mapped);
}

#ifdef _WIN32
struct opaque_buffer_copier::impl : gpu
{
	using gpu::gpu;
	VkImage image = VK_NULL_HANDLE; // the D3D11 texture array
	VkDeviceMemory image_memory = VK_NULL_HANDLE;
	struct imported
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
	};
	std::map<uint64_t, imported> buffers; // by dmabuf_image::id
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	uint32_t width = 0, height = 0, layers = 0;
	bool ten_bit = false;

	void drop(imported & b)
	{
		vkDestroyBuffer(device, b.buffer, nullptr);
		vkFreeMemory(device, b.memory, nullptr);
	}
	~impl()
	{
		vkDeviceWaitIdle(device);
		for (auto & [id, b]: buffers)
			drop(b);
		vkDestroyImage(device, image, nullptr);
		vkFreeMemory(device, image_memory, nullptr);
		vkDestroyFence(device, fence, nullptr);
		vkDestroyCommandPool(device, pool, nullptr);
	}
};

opaque_buffer_copier::opaque_buffer_copier(const std::string & node, void * texture, uint32_t w, uint32_t h, uint32_t layers, bool ten_bit) :
        p(std::make_unique<impl>(node))
{
	p->width = w;
	p->height = h;
	p->layers = layers;
	p->ten_bit = ten_bit;
	// the encoder's D3D11 frame pool as a Vulkan image (same memory)
	VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = ten_bit ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = layers;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	check(vkCreateImage(p->device, &ici, nullptr, &p->image), "vkCreateImage(D3D11 frame pool)");
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(p->device, p->image, &req);
	VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	imp.handle = texture;
	VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
	ded.image = p->image;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = memory_type(p->memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	check(vkAllocateMemory(p->device, &mai, nullptr, &p->image_memory), "import the D3D11 frame pool");
	check(vkBindImageMemory(p->device, p->image, p->image_memory, 0), "vkBindImageMemory(D3D11 frame pool)");
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = p->family;
	check(vkCreateCommandPool(p->device, &pci, nullptr, &p->pool), "vkCreateCommandPool");
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = p->pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	check(vkAllocateCommandBuffers(p->device, &cai, &p->cmd), "vkAllocateCommandBuffers");
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	check(vkCreateFence(p->device, &fci, nullptr, &p->fence), "vkCreateFence");
}

opaque_buffer_copier::~opaque_buffer_copier() = default;

void opaque_buffer_copier::copy(native_handle memory, uint64_t size, uint64_t buffer_bytes, uint64_t buffer_id, uint32_t stride,
                                uint32_t uv_offset, uint32_t layer)
{
	auto it = p->buffers.find(buffer_id);
	if (it == p->buffers.end() && p->buffers.size() >= 12) // the runtime's pool was replaced
	{
		vkDeviceWaitIdle(p->device);
		for (auto & [id, b]: p->buffers)
			p->drop(b);
		p->buffers.clear();
	}
	if (it == p->buffers.end())
	{
		impl::imported b;
		b.buffer = create_buffer(p->device, buffer_bytes, ipc::opaque_buffer_usage); // identical to the exporter's
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(p->device, b.buffer, &req);
		VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
		imp.handleType = external_handle_type;
		imp.handle = memory;
		VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
		ded.buffer = b.buffer;
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
		mai.allocationSize = size;
		mai.memoryTypeIndex = memory_type(p->memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (VkResult r = vkAllocateMemory(p->device, &mai, nullptr, &b.memory); r != VK_SUCCESS)
		{
			vkDestroyBuffer(p->device, b.buffer, nullptr);
			check(r, "vkAllocateMemory(import VR buffer)");
		}
		vkBindBufferMemory(p->device, b.buffer, b.memory, 0);
		it = p->buffers.emplace(buffer_id, b).first;
	}
	vkResetCommandBuffer(p->cmd, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(p->cmd, &bi);
	VkBufferMemoryBarrier acquire{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
	acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	acquire.dstQueueFamilyIndex = p->family;
	acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	acquire.buffer = it->second.buffer;
	acquire.size = VK_WHOLE_SIZE;
	VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // the slice is fully rewritten
	to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_dst.srcQueueFamilyIndex = to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_dst.image = p->image;
	to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
	vkCmdPipelineBarrier(p->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &acquire, 1, &to_dst);
	const uint32_t sample = p->ten_bit ? 2 : 1; // bytes per luma sample
	VkBufferImageCopy regions[2]{};
	regions[0].bufferOffset = 0;
	regions[0].bufferRowLength = stride / sample;
	regions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, layer, 1};
	regions[0].imageExtent = {p->width, p->height, 1};
	regions[1].bufferOffset = uv_offset;
	regions[1].bufferRowLength = stride / (2 * sample); // interleaved CbCr texels
	regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, layer, 1};
	regions[1].imageExtent = {p->width / 2, p->height / 2, 1};
	vkCmdCopyBufferToImage(p->cmd, it->second.buffer, p->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);
	VkImageMemoryBarrier release = to_dst; // to D3D11 (the encoder) next
	release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	release.dstAccessMask = 0;
	release.srcQueueFamilyIndex = p->family;
	release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	vkCmdPipelineBarrier(p->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &release);
	vkEndCommandBuffer(p->cmd);
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &p->cmd;
	vkResetFences(p->device, 1, &p->fence);
	check(vkQueueSubmit(p->queue, 1, &si, p->fence), "vkQueueSubmit(copy to the encoder)");
	vkWaitForFences(p->device, 1, &p->fence, VK_TRUE, UINT64_MAX);
}
#endif
} // namespace od

static_assert(od::ipc::opaque_buffer_usage ==
              (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));