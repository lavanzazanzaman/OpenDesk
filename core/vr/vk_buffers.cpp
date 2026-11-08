#include "vk_buffers.h"
#include "vr_ipc.h"

#include <atomic>
#include <cstring>
#include <format>
#include <map>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

namespace od
{

namespace
{
constexpr auto external_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr const char * external_memory_extension = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;

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

// an instance + device on the GPU behind a DRM render node, with external memory support
struct gpu
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkPhysicalDeviceMemoryProperties memory{};
	std::array<uint8_t, 16> device_uuid{}, driver_uuid{};
	PFN_vkGetMemoryFdKHR get_fd = nullptr;

	explicit gpu(const std::string & node)
	{
		struct stat st{};
		if (stat(node.c_str(), &st) != 0)
			throw std::runtime_error("cannot stat " + node);
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
			VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
			VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &drm};
			VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
			vkGetPhysicalDeviceProperties2(d, &props);
			if (drm.hasRender && uint32_t(drm.renderMajor) == major(st.st_rdev) && uint32_t(drm.renderMinor) == minor(st.st_rdev))
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
		get_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
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
	bool host_visible = false;
};

vk_buffer_pool::vk_buffer_pool(const std::string & node, bool host_visible) :
        p(std::make_shared<impl>(node))
{
	p->host_visible = host_visible;
	if (!p->get_fd)
		throw std::runtime_error("vkGetMemoryFdKHR unavailable");
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
		if (i->fd >= 0)
			close(i->fd);
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
	// an encoder on another GPU: system memory the compositor writes and the encoder's CPU side reads, no extra GPU copy
	static constexpr VkMemoryPropertyFlags host_flags =
	        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	mai.memoryTypeIndex = memory_type(p->memory, req.memoryTypeBits, p->host_visible ? host_flags : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	img->host_visible = p->host_visible && (p->memory.memoryTypes[mai.memoryTypeIndex].propertyFlags & host_flags) == host_flags;
	VkDeviceMemory memory;
	if (VkResult r = vkAllocateMemory(p->device, &mai, nullptr, &memory); r != VK_SUCCESS)
	{
		vkDestroyBuffer(p->device, buffer, nullptr);
		check(r, "vkAllocateMemory(VR buffer)");
	}
	vkBindBufferMemory(p->device, buffer, memory, 0);
	VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gfi.memory = memory;
	gfi.handleType = external_handle_type;
	int fd = -1;
	VkResult r = p->get_fd(p->device, &gfi, &fd);
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
		void * mapped = nullptr; // system-memory buffers: read in place
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

const uint8_t * opaque_buffer_reader::read(native_handle fd, uint64_t size, uint64_t buffer_bytes, uint64_t buffer_id, bool host_visible)
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
		VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
		imp.handleType = external_handle_type;
		imp.fd = dup(fd); // Vulkan owns it on success
		VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
		ded.buffer = b.buffer;
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
		mai.allocationSize = size;
		mai.memoryTypeIndex = memory_type(p->memory, req.memoryTypeBits,
		                                  host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT
		                                               : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (VkResult r = vkAllocateMemory(p->device, &mai, nullptr, &b.memory); r != VK_SUCCESS)
		{
			close(imp.fd);
			vkDestroyBuffer(p->device, b.buffer, nullptr);
			check(r, "vkAllocateMemory(import VR buffer)");
		}
		vkBindBufferMemory(p->device, b.buffer, b.memory, 0);
		if (host_visible)
			check(vkMapMemory(p->device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory(VR buffer)");
		it = p->buffers.emplace(buffer_id, b).first;
	}
	if (it->second.mapped) // the compositor's work is done before the streamer gets the frame
		return static_cast<const uint8_t *>(it->second.mapped);
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

} // namespace od

static_assert(od::ipc::opaque_buffer_usage ==
              (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));