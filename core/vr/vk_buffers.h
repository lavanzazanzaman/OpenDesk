// VR stream buffers as exportable Vulkan memory (OPAQUE_FD; on Windows OPAQUE_WIN32 NT handles, on every GPU) instead of GBM DMA-BUFs: what NVIDIA needs (its GBM
// has no linear render buffers, and NVENC reads CUDA memory, which imports OPAQUE_FD but not DMA-BUF). Same idea as
// WiVRn's NVENC path. Each buffer holds an NV12/P010 frame; the runtime's compositor converts its composited image
// into it, the encoder imports the same memory (CUDA for NVENC, a Vulkan readback otherwise).
#pragma once

#include "dmabuf.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace od
{
class vk_buffer_pool
{
public:
	explicit vk_buffer_pool(const std::string & render_node); // throws if the GPU can't export OPAQUE_FD memory
	~vk_buffer_pool();
	vk_buffer_pool(const vk_buffer_pool &) = delete;
	vk_buffer_pool & operator=(const vk_buffer_pool &) = delete;

	// memory = memory_opaque_buffer, NV12 (or P010 when ten_bit) with the chroma plane right after the luma rows (what
	// NVENC expects); fd: the exported memory (owned by the image), size: the allocation size
	std::unique_ptr<dmabuf_image, void (*)(dmabuf_image *)> allocate(uint32_t width, uint32_t height, bool ten_bit);
	const std::array<uint8_t, 16> & device_uuid() const;
	const std::array<uint8_t, 16> & driver_uuid() const;

private:
	struct impl;
	std::shared_ptr<impl> p; // images keep it alive
};

// CPU access to an OPAQUE_FD buffer (encoders without CUDA): imports the memory once per buffer and copies it to a
// host-visible staging buffer on read
class opaque_buffer_reader
{
public:
	explicit opaque_buffer_reader(const std::string & render_node);
	~opaque_buffer_reader();
	const uint8_t * read(native_handle fd, uint64_t size, uint64_t buffer_bytes, uint64_t buffer_id);

private:
	struct impl;
	std::unique_ptr<impl> p;
};
#ifdef _WIN32
// Windows, encoders that take D3D11 frames (AMF, Media Foundation): copies an OPAQUE_WIN32 stream buffer (NV12/P010
// rows, chroma at uv_offset) into one slice of a shared D3D11 texture array (the encoder's frame pool) with a GPU copy,
// so the frame never passes through system memory
class opaque_buffer_copier
{
public:
	// texture: the NT handle of a shared NV12/P010 texture array of `layers` slices (D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
	opaque_buffer_copier(const std::string & render_node, void * texture, uint32_t width, uint32_t height, uint32_t layers, bool ten_bit);
	~opaque_buffer_copier();
	// returns once the copy finished on the GPU
	void copy(native_handle memory, uint64_t size, uint64_t buffer_bytes, uint64_t buffer_id, uint32_t stride, uint32_t uv_offset,
	          uint32_t layer);

private:
	struct impl;
	std::unique_ptr<impl> p;
};
#endif
} // namespace od
