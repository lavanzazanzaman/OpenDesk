// XRGB8888 DMA-BUF images on the VR GPU (GBM): linear where the driver allows (VAAPI imports them zero-copy), else a
// tiled layout the runtime's compositor can write as a storage image (NVIDIA's GBM has no linear render buffers).
// Producers (the built-in test pattern, the OpenXR runtime) render into them.
#pragma once

#include "native_handle.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct gbm_device;
struct gbm_bo;

namespace od
{
struct dmabuf_image
{
	gbm_bo * bo = nullptr;
	native_handle fd = no_handle; // DMA-BUF / OPAQUE_FD (vk_buffers.h)
	uint32_t width = 0, height = 0, stride = 0, offset = 0;
	uint32_t fourcc = 0; // DRM_FORMAT_XRGB8888
	uint64_t modifier = 0; // DRM_FORMAT_MOD_LINEAR
	uint32_t memory = 0;   // ipc::memory_dmabuf / memory_opaque_buffer (vk_buffers.h)
	uint64_t size = 0;     // opaque buffers: the allocation size
	uint32_t uv_offset = 0; // opaque buffers (NV12/P010): chroma plane offset
	uint64_t buffer_bytes = 0; // opaque buffers: the VkBuffer size (importers must create theirs identically)
	bool host_visible = false; // opaque buffers in system memory (another GPU encodes them)
	std::function<void()> release; // frees what owns the memory (opaque buffers)
	uint64_t id = next_id();       // unique per allocation (fd numbers get reused)
	static uint64_t next_id();
};

class gbm_allocator
{
public:
	explicit gbm_allocator(const std::string & render_node);
	~gbm_allocator();
	gbm_allocator(const gbm_allocator &) = delete;
	gbm_allocator & operator=(const gbm_allocator &) = delete;

	gbm_device * device() const { return gbm; }
	std::unique_ptr<dmabuf_image, void (*)(dmabuf_image *)> allocate(uint32_t width, uint32_t height);

private:
	int drm_fd = -1;
	gbm_device * gbm = nullptr;
	std::string node_name;
	std::vector<uint64_t> storage_modifiers; // R32_UINT storage-capable layouts on this GPU (Vulkan), queried on demand
	bool queried = false;
	std::string query_detail;
};
} // namespace od
