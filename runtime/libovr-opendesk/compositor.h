// libovr-opendesk compositor. Swapchain images are plain VkImages on the application's device (the app renders into them);
// at EndFrame the layers are composited per eye by a compute shader straight into the streamer's DMA-BUF images,
// already in the headset's defoveation-warped layout.
//
// The compositing runs on a private VkDevice of the same GPU, not on the app's: importing DMA-BUFs needs
// VK_EXT_external_memory_dma_buf + VK_EXT_image_drm_format_modifier, which Wine does not expose to Windows games
// (DXVK/vkd3d then fail to create their device). The app's device only needs VK_KHR_external_memory_fd: swapchain
// memory is exported as an opaque fd and imported into the private device (same driver, identical image parameters).
// Per frame the images are released from the app's queue, composited, and handed back (queue family ownership via
// VK_QUEUE_FAMILY_EXTERNAL).
#pragma once

#include "vr_client.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace od::ovr
{
void report(const char * fmt, ...); // stderr + the streamer's log (ovr_vd.cpp)

// "Use Fov Stencil": the lens-shaped ellipse (in viewing angle, inscribed in the headset's FOV) grown by this factor;
// outside it is the hidden area games may skip (ovr_GetFovStencil) and the compositor sends black. 1.2 hides about 9%
// of the image (1.05 hid 24% and cut into what the lenses show; stencils made for Quest headsets hide 6-14%).
constexpr float fov_stencil_margin = 1.2f;

struct swapchain
{
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t width = 0, height = 0, layers = 1;
	uint32_t samples = 1, mips = 1;
	bool depth = false;
	bool foreign = false;                 // an overlay app's images (no `images`/`memory`: only `shared`)
	std::vector<uint8_t> external;        // overlay app: which images are handed to the game drawing them
	std::vector<VkImage> images;          // on the app's device (what the app renders into)
	std::vector<VkDeviceMemory> memory;
	std::vector<uint64_t> memory_size;    // of each entry in `memory`
	std::vector<VkImage> shared;          // the same memory on the compositor's device (color only)
	std::vector<VkDeviceMemory> shared_memory;
	std::vector<std::vector<VkImageView>> views; // [image][array layer] of `shared`
	int current = 0;   // next index the app renders into
	int committed = -1; // last committed index (what the compositor samples)
};

struct composite_layer
{
	enum kind
	{
		projection,
		quad,
		cylinder,
		cube,
		equirect,
	} type;
	const swapchain * chain[2] = {}; // per eye (quad: same for both)
	uint32_t array_index[2] = {};
	float rect[2][4] = {};           // normalized sub-rectangle per eye
	float fov[2][4] = {};            // projection: left, right, up, down tangents
	// quad: pose in tracking space (or head space if head_locked), size in meters
	std::array<float, 4> quad_orientation{0, 0, 0, 1};
	std::array<float, 3> quad_position{};
	float quad_size[2] = {1, 1};
	float cylinder_radius = 1, cylinder_angle = 1, cylinder_aspect = 1; // equirect: radius and horizontal angle too
	float equirect_upper = 0, equirect_lower = 0;
	int cube_face = 0;
	bool head_locked = false;
	bool origin_bottom_left = false;
	bool premultiplied = true; // OpenXR default
	float alpha = 1.f; // the layer's alpha scale (XR_KHR_composition_layer_color_scale_bias)
	bool opaque = false;        // ignore the source alpha
	uint32_t eye_mask = 3; // bit 0 left, bit 1 right
};

class compositor
{
public:
	// the application's GPU objects (its session binding)
	compositor(VkPhysicalDevice, VkDevice, VkQueue, uint32_t queue_family);
	~compositor();

	std::unique_ptr<swapchain> create_swapchain(VkFormat, uint32_t width, uint32_t height, uint32_t array_size,
	                                            uint32_t mip_levels, uint32_t sample_count, uint32_t image_count, bool depth, bool cube = false);
	void destroy_swapchain(swapchain &);
	// an overlay app's swapchain, from the fds of its images (takes them); throws on failure
	std::unique_ptr<swapchain> import_foreign(const ipc::link_swapchain &, const std::vector<int> & fds);
	// overlay app: hands an image to / takes it back from the game that samples it (waits for the app's rendering)
	void hand_over(swapchain &, size_t image, bool to_game);
	int export_memory(const swapchain & sc, size_t index); // a new fd for the image's memory (color images only)

	// composites into the client's slot images; blocks until the GPU finished. eye_pose: the frame's render poses
	// (tracking space) - quads are placed relative to them. stream_fov: left, right, up, down per eye.
	// GPU-side sync available: composite() returns once the work is queued; wait_composited() before handing the slot
	// to the streamer (any thread)
	bool pipelined() const { return gpu_sync; }
	bool wait_composited(uint32_t slot);
	bool composite(vr_client &, uint32_t slot, const std::vector<composite_layer> &, const float stream_fov[2][4],
	               const std::array<float, 4> eye_orientation[2], const std::array<float, 3> eye_position[2],
	               const std::array<float, 4> & head_orientation, const std::array<float, 3> & head_position,
	               const int32_t fovea_top_left[2][2] = nullptr); // foveated: where each eye's fovea crop sits

private:
	struct output
	{
		VkImage image = VK_NULL_HANDLE; // the streamer's DMA-BUF
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE; // direct mode: the shader writes the DMA-BUF
		// copy mode (the DMA-BUF's layout takes no storage writes, e.g. NVIDIA's tiled ones): the shader writes this
		// private image, which is then copied into the DMA-BUF
		VkImage work = VK_NULL_HANDLE;
		VkDeviceMemory work_memory = VK_NULL_HANDLE;
		VkImageView work_view = VK_NULL_HANDLE;
		// opaque-buffer mode (the streamer's Vulkan memory, NVIDIA): `work` is copied into this buffer instead
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDescriptorSet yuv_set = VK_NULL_HANDLE; // work image -> buffer (yuv.comp)
		uint32_t pitch_words = 0, uv_offset_words = 0, p010 = 0, full_range = 0;
		bool host_visible = false; // the buffer is system memory (another GPU encodes it): make the writes visible to the CPU
		uint32_t width = 0, height = 0;
		VkImage target() const { return work ? work : image; }
		VkImageView target_view() const { return work ? work_view : view; }
	};
	void import_outputs(vr_client &);

	// GPU-side synchronisation with the app's queue (no CPU waits in ovr_EndFrame): per slot, a semaphore pair shared
	// between the app's device and ours (OPAQUE_FD) plus our own command buffers, fences and descriptor pool, so frames
	// in flight never share objects. A slot is reused only after the streamer released it, i.e. after its GPU work.
	struct frame_slot
	{
		VkCommandBuffer app_release = VK_NULL_HANDLE, app_acquire = VK_NULL_HANDLE, cmd = VK_NULL_HANDLE;
		VkFence app_fence = VK_NULL_HANDLE, fence = VK_NULL_HANDLE;
		bool app_pending = false, pending = false;
		VkSemaphore ready_app = VK_NULL_HANDLE, ready = VK_NULL_HANDLE; // app rendering done -> compositor
		VkSemaphore done = VK_NULL_HANDLE, done_app = VK_NULL_HANDLE;   // compositor done -> app may render again
		VkDescriptorPool descriptors = VK_NULL_HANDLE;
	};
	frame_slot frames[ipc::max_slots];
	bool gpu_sync = false;
	bool private_semaphore_fd = false; // our device has VK_KHR_external_semaphore_fd
	void create_gpu_sync();
	void destroy_gpu_sync();
	uint32_t per_slot = 2;       // outputs per slot: left, right (+ left fovea, right fovea)
	uint32_t eye_size[2] = {1, 1}; // the eye render target the fovea crops are taken from
	uint32_t periphery_size[2] = {0, 0}; // foveated: the periphery stream image (the fovea's edge imitates it)
	void release_outputs();
	static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties &, uint32_t bits, VkMemoryPropertyFlags);
	void one_time_submit(void (*record)(VkCommandBuffer, void *), void * ctx);     // on the compositor's device
	// the compositor's queue (DXVK mode: DXVK's, used under its submission lock)
	VkResult queue_submit(const VkSubmitInfo &, VkFence);
	void queue_idle();
	void app_submit(void (*record)(VkCommandBuffer, void *), void * ctx);         // on the app's queue, waits
	void create_private_device();

	// the application's device (D3D11 mode: none)
	VkPhysicalDevice app_physical = VK_NULL_HANDLE;
	VkDevice app_device = VK_NULL_HANDLE;
	VkQueue app_queue = VK_NULL_HANDLE;
	uint32_t app_family = 0;
	VkPhysicalDeviceMemoryProperties app_memory_properties{};
	VkCommandPool app_pool = VK_NULL_HANDLE;
	VkCommandBuffer app_cmd = VK_NULL_HANDLE;
	VkFence app_fence = VK_NULL_HANDLE;
	PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;
	bool app_vulkan() const { return app_device != VK_NULL_HANDLE; }

	// the compositor's private device on the same GPU
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkPhysicalDeviceMemoryProperties memory_properties{};
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	void init_pipelines(); // everything on the private device after it exists
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkPipeline pipeline_ms = VK_NULL_HANDLE;
	// NV12/P010 conversion into the streamer's Vulkan buffers
	VkDescriptorSetLayout yuv_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout yuv_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline yuv_pipeline = VK_NULL_HANDLE;
	VkDescriptorPool yuv_pool = VK_NULL_HANDLE;
	void create_yuv_pipeline();
	VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
	VkImage black = VK_NULL_HANDLE; // bound when a layer has nothing to sample (clear pass)
	VkDeviceMemory black_memory = VK_NULL_HANDLE;
	VkImageView black_view = VK_NULL_HANDLE;
	uint32_t generation = 0;
	std::vector<output> outputs; // [slot * 2 + eye]
	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;
};

// device extensions the app's VkDevice needs (ovr_GetDeviceExtensionsVk / xrGetVulkanDeviceExtensionsKHR); all of
// them are available to Windows games under Wine
const char * required_device_extensions();
VkFormat vk_format_for(int ovr_format, bool & depth, bool & srgb);
} // namespace od::ovr
