#include "dmabuf.h"

#include <fcntl.h>
#include <gbm.h>
#include <cerrno>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

namespace od
{
uint64_t dmabuf_image::next_id()
{
	static std::atomic<uint64_t> n{0};
	return ++n;
}

namespace
{
// DRM format modifiers with which the GPU behind `node` can use an R32_UINT image (the compositor's view of
// XRGB8888) as a storage image, single plane
std::vector<uint64_t> query_storage_modifiers(const std::string & node, std::string & why)
{
	std::vector<uint64_t> out;
	struct stat st{};
	if (stat(node.c_str(), &st) != 0)
		return why = "cannot stat " + node, out;
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance instance;
	if (VkResult r = vkCreateInstance(&ici, nullptr, &instance); r != VK_SUCCESS)
		return why = std::format("vkCreateInstance failed ({})", int(r)), out;
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(instance, &n, nullptr);
	std::vector<VkPhysicalDevice> devices(n);
	vkEnumeratePhysicalDevices(instance, &n, devices.data());
	for (VkPhysicalDevice d: devices)
	{
		uint32_t count = 0;
		vkEnumerateDeviceExtensionProperties(d, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> ext(count);
		vkEnumerateDeviceExtensionProperties(d, nullptr, &count, ext.data());
		auto has = [&](const char * name) {
			for (auto & e: ext)
				if (!std::strcmp(e.extensionName, name))
					return true;
			return false;
		};
		if (!has(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) || !has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME))
		{
			VkPhysicalDeviceProperties pp;
			vkGetPhysicalDeviceProperties(d, &pp);
			why += std::format("{} lacks DRM extensions; ", pp.deviceName);
			continue;
		}
		VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
		VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drm};
		vkGetPhysicalDeviceProperties2(d, &props);
		if (!drm.hasRender || uint32_t(drm.renderMajor) != major(st.st_rdev) || uint32_t(drm.renderMinor) != minor(st.st_rdev))
			continue;
		VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
		VkFormatProperties2 fp{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &list};
		// the compositor writes the buffer as an R32_UINT storage image, or copies into it (R32_UINT or BGRA8)
		size_t direct = 0;
		for (VkFormat format: {VK_FORMAT_R32_UINT, VK_FORMAT_B8G8R8A8_UNORM})
		{
			list.drmFormatModifierCount = 0;
			list.pDrmFormatModifierProperties = nullptr;
			vkGetPhysicalDeviceFormatProperties2(d, format, &fp);
			std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
			list.pDrmFormatModifierProperties = mods.data();
			vkGetPhysicalDeviceFormatProperties2(d, format, &fp);
			for (const auto & m: mods)
				if (m.drmFormatModifierPlaneCount == 1 &&
				    (m.drmFormatModifierTilingFeatures & (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) &&
				    std::find(out.begin(), out.end(), m.drmFormatModifier) == out.end())
				{
					// layouts the shader writes directly first, copy-only ones after
					if (format == VK_FORMAT_R32_UINT && (m.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
						out.insert(out.begin() + ptrdiff_t(direct++), m.drmFormatModifier);
					else
						out.push_back(m.drmFormatModifier);
				}
		}
		why += std::format("{}: {} layouts ({} direct); ", props.properties.deviceName, out.size(), direct);
		break;
	}
	if (why.empty())
		why = std::format("no Vulkan device for {} ({} devices)", node, n);
	vkDestroyInstance(instance, nullptr);
	return out;
}
} // namespace

gbm_allocator::gbm_allocator(const std::string & node)
{
	drm_fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
	if (drm_fd < 0)
		throw std::runtime_error("cannot open " + node);
	node_name = node;
	gbm = gbm_create_device(drm_fd);
	if (!gbm)
		throw std::runtime_error("gbm_create_device failed on " + node);
}

gbm_allocator::~gbm_allocator()
{
	if (gbm)
		gbm_device_destroy(gbm);
	if (drm_fd >= 0)
		close(drm_fd);
}

std::unique_ptr<dmabuf_image, void (*)(dmabuf_image *)> gbm_allocator::allocate(uint32_t w, uint32_t h)
{
	auto deleter = [](dmabuf_image * i) {
		if (i->fd >= 0)
			close(i->fd);
		if (i->bo)
			gbm_bo_destroy(i->bo);
		if (i->release)
			i->release();
		delete i;
	};
	std::unique_ptr<dmabuf_image, void (*)(dmabuf_image *)> img(new dmabuf_image, deleter);
	static const bool test_tiled = [] { // harness: NVIDIA's path on any GPU
		const char * e = std::getenv("OPENDESK_TEST_TILED_VR_BUFFERS");
		return e && *e && *e != '0';
	}();
	if (!test_tiled)
		img->bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	if (!img->bo) // NVIDIA: no linear render buffers - one of its tiled layouts the compositor can write instead
	{
		if (!queried)
		{
			storage_modifiers = query_storage_modifiers(node_name, query_detail);
			queried = true;
		}
		// one at a time in preference order: GBM picks freely among a list
		for (uint64_t m: storage_modifiers)
			if (m != 0 /* linear: refused above */ && !img->bo)
				img->bo = gbm_bo_create_with_modifiers2(gbm, w, h, GBM_FORMAT_XRGB8888, &m, 1, GBM_BO_USE_RENDERING);
		if (!img->bo) // nothing known: the driver's own choice (the compositor finds out how to write it)
			img->bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
	}
	if (!img->bo)
		throw std::runtime_error(std::format("gbm_bo_create {}x{} XRGB8888 failed on {} ({}; Vulkan: {}): {}", w, h, node_name,
		                                     gbm_device_get_backend_name(gbm), query_detail, strerror(errno)));
	img->fd = gbm_bo_get_fd(img->bo);
	img->width = w;
	img->height = h;
	img->stride = gbm_bo_get_stride(img->bo);
	img->offset = gbm_bo_get_offset(img->bo, 0);
	img->fourcc = GBM_FORMAT_XRGB8888;
	img->modifier = gbm_bo_get_modifier(img->bo);
	if (img->modifier == 0x00ffffffffffffffull) // DRM_FORMAT_MOD_INVALID: the plain linear allocation
		img->modifier = 0;
	if (img->fd < 0)
		throw std::runtime_error("gbm_bo_get_fd failed");
	return img;
}
} // namespace od
