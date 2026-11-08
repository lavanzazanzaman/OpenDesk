// In-process low-latency VAAPI encoder (FFmpeg): captured RGB frames -> NV12 on the GPU -> H.264/HEVC access units.
//   DMA-BUF input: DRM-PRIME frame -> hwmap(derive_device=vaapi) -> scale_vaapi(nv12) -> {h264,hevc}_vaapi (zero copy)
//   memory input : bgr0/bgra frame -> hwupload -> scale_vaapi(nv12) -> encoder
// Every encoded packet is one complete access unit (SPS/PPS in-band on IDRs), so no Annex-B splitting is needed.
#pragma once

#include "bytes.h"
#include "native_handle.h"
#include "video.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AVBufferRef;
struct AVCodec;
struct AVCodecContext;
struct AVFilterGraph;
struct AVFilterContext;
struct AVFrame;
struct SwsContext;

namespace od
{
struct encoder_config
{
	int width = 0, height = 0;
	int fps = 60;
	int kbps = 20000;
	video_codec codec = video_codec::h264;
	std::string render_node; // /dev/dri/renderD*: the GPU that encodes
	bool full_range = false;  // full-range YUV + VUI flag (headset: IncreaseVideoNominalRange)
	// NVENC options (and their AMD VAAPI equivalents through the quality-level bitfield):
	bool pre_encode = false; // "2-Pass encoding": low-res pre-analysis pass for better rate control
	bool vbaq = false;       // "Adaptive quantization": variance-based adaptive quantization
	int preset = 4;          // NVENC p1 (fastest) .. p7
	std::string source_node; // the GPU the frames are on, when another one renders them (empty: render_node)
	int amd_preset = 0;      // AMD VAAPI quality preset: 0 speed, 1 balanced, 2 quality
	bool operator==(const encoder_config &) const = default;
};

struct captured_frame
{
	// DMA-BUF / Vulkan memory or CPU memory (data != nullptr)
	native_handle fd = no_handle;
	uint32_t drm_format = 0; // DRM fourcc, e.g. XRGB8888
	uint64_t modifier = 0;
	uint32_t offset = 0, stride = 0;
	size_t size = 0;
	const uint8_t * data = nullptr;
	int av_sw_format = -1; // AVPixelFormat of the memory layout (bgr0, bgra, ...)
	bool opaque_buffer = false; // fd: Vulkan OPAQUE_FD memory of `size` bytes, rows of `stride` (vk_buffers.h)
	uint64_t buffer_id = 0;     // identifies the allocation behind fd (imports are cached by it)
	uint32_t uv_offset = 0;     // two-plane formats (NV12/P010): chroma plane offset
	uint64_t buffer_bytes = 0;  // opaque buffers: the exporter's VkBuffer size
	bool host_visible = false;  // opaque buffers in system memory: mapped directly
};

class opaque_buffer_reader;

class hardware_encoder
{
public:
	using au_callback = std::function<void(bytes && au, bool sync)>;

	explicit hardware_encoder(encoder_config);
	~hardware_encoder();
	hardware_encoder(const hardware_encoder &) = delete;
	hardware_encoder & operator=(const hardware_encoder &) = delete;

	const encoder_config & config() const { return cfg; }
	const char * backend() const
	{
		return nvenc ? "NVENC" : "VAAPI";
	}
	// e.g. "NVENC, CUDA input" / "VAAPI, CPU copy": how frames reach the encoder (after prime)
	std::string input_path() const;
	void set_log(std::function<void(const std::string &)> l) { log = std::move(l); }

	// throws std::runtime_error on setup failures
	void encode(const captured_frame &, bool force_idr, const au_callback &);
	// sets up the filter graph and opens the codec for this frame layout (throws if unsupported)
	void prime(const captured_frame & f);
	// converts a captured frame to the encoder's surface without encoding it yet (the buffer can go back to the
	// compositor right after); encode it with reencode_last
	void stage(const captured_frame &);
	// encode the last staged/encoded frame (new frame, or a repeat while the screen is static)
	bool reencode_last(bool force_idr, const au_callback &);

private:
	std::function<void(const std::string &)> log;
	void note(const std::string & line);
	encoder_config cfg;
	void setup_filters(const captured_frame &);
	void encode_nv12(AVFrame *, bool force_idr, const au_callback &);
	void setup_nvenc(const captured_frame &);
	void stage_nvenc(const captured_frame &);
	const captured_frame & host_frame(const captured_frame &, captured_frame & storage); // opaque buffer -> CPU copy
	struct readback;
	std::unique_ptr<readback> download;
	std::unique_ptr<opaque_buffer_reader> opaque; // Vulkan OPAQUE_FD buffers without CUDA
	struct cuda_state;                            // NVENC: OPAQUE_FD buffers imported into CUDA, encoded in place
	std::unique_ptr<cuda_state> cuda;
	bool cuda_input = false, cuda_upload = false; // CUDA frames imported in place, or (frames on another GPU) uploaded
	bool cuda_failed = false; // the CUDA input path failed once: CPU copies from then on
	bool cpu_copy = false;    // the last staged frame went through a CPU copy
	SwsContext * sws = nullptr;
	bool nvenc = false;

	AVBufferRef * vaapi_device = nullptr;
	AVBufferRef * drm_device = nullptr;
	AVBufferRef * drm_frames = nullptr;
	AVFilterGraph * graph = nullptr;
	AVFilterContext * src = nullptr;
	AVFilterContext * sink = nullptr;
	bool graph_is_dmabuf = false;
	int graph_sw_format = -1;
	AVCodecContext * codec = nullptr;
	AVFrame * last_nv12 = nullptr;
	int64_t pts_origin = -1;
	int64_t last_pts = -1;
};

// Picks the render node of the GPU driving the displays (OPENDESK_RENDER_NODE overrides).
std::string default_render_node();
// The GPU for VR (apps render there, its buffers and encoder live there): the display GPU, except on hybrid laptops
// whose display runs on an integrated GPU, where the discrete one (OPENDESK_VR_RENDER_NODE overrides).
std::string vr_render_node();
// the GPU encoding VR (vr_render_node() unless two cards are in use) and the desktop (always the display's)
std::string vr_encode_node();
std::string desktop_encode_node();
// the card VR games use with no card chosen: the main screen's (a laptop's discrete GPU if the screen is on the integrated one)
std::string auto_vr_render_node();
// dual-GPU use (PCI addresses like 0000:03:00.0; empty: automatic / one card for everything)
void set_gpu_selection(const std::string & game_pci, const std::string & streaming_pci);
struct gpu_info
{
	std::string node, pci, name;
	unsigned vendor = 0;
};
std::vector<gpu_info> list_gpus();
unsigned gpu_vendor(const std::string & render_node);
// Opens the VAAPI device and looks for the H.264 encoder.
// Empty string = usable, else the reason.
std::string hardware_encoder_probe(const std::string & render_node);
// The codecs the GPU can encode (one small frame each; takes a moment - call off the UI thread)
std::vector<video_codec> hardware_encoder_codecs(const std::string & render_node);
} // namespace od
