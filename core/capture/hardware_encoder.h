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

#ifdef _WIN32
extern "C" {
#include <libavutil/pixfmt.h> // AVPixelFormat in the Windows encoder members
}
#endif

namespace od
{
struct encoder_config
{
	int width = 0, height = 0;
	int fps = 60;
	int kbps = 20000;
	video_codec codec = video_codec::h264;
	std::string render_node; // /dev/dri/renderD*; must be the GPU the compositor renders on
	bool full_range = false;  // full-range YUV + VUI flag (headset: IncreaseVideoNominalRange)
	// NVENC options (and their AMD VAAPI equivalents through the quality-level bitfield):
	bool pre_encode = false; // "2-Pass encoding": low-res pre-analysis pass for better rate control
	bool vbaq = false;       // "Adaptive quantization": variance-based adaptive quantization
	int preset = 4;          // NVENC p1 (fastest) .. p7
	bool operator==(const encoder_config &) const = default;
};

struct captured_frame
{
	// DMA-BUF / Vulkan memory (fd: an fd on Linux, an NT handle on Windows) or CPU memory (data != nullptr)
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
#ifdef _WIN32
	// Windows desktop capture on the GPU: a BGRA ID3D11Texture2D on this ID3D11Device (valid during the call)
	void * d3d11_device = nullptr;
	void * d3d11_texture = nullptr;
#endif
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
#ifdef _WIN32
		return selected ? selected : "FFmpeg";
#else
		return nvenc ? "NVENC" : "VAAPI";
#endif
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
#ifdef _WIN32
	// Windows: BGRA frames from DXGI Desktop Duplication or NV12/P010 VR frames (Vulkan memory from the runtime) on the
	// GPU named by cfg.render_node -> the first working hardware encoder (NVENC / AMF / QSV) or Media Foundation /
	// libx264 / libx265. NVENC takes VR frames straight from the Vulkan memory through CUDA; the others get a GPU readback.
	void setup(const captured_frame &);
	void open_codec(AVPixelFormat input, AVBufferRef * frames);
	void convert(const captured_frame &);
	void send_frame(bool force_idr, const au_callback &);
	const AVCodec * codec_def = nullptr;
	AVCodecContext * codec = nullptr;
	const char * selected = nullptr;
	AVFrame * last = nullptr;
	AVFrame * nv12 = nullptr;
	SwsContext * sws = nullptr;
	int src_width = 0, src_height = 0, src_stride = 0;
	AVPixelFormat src_format = AV_PIX_FMT_NONE;
	bool src_opaque = false;
	int64_t pts = 0;
	AVBufferRef * hw_device = nullptr; // D3D11 device on the chosen adapter (picks the GPU for NVENC / AMF / QSV)
	struct cuda_state;                 // NVENC: the runtime's Vulkan memory imported into CUDA, encoded in place
	std::unique_ptr<cuda_state> cuda;
	bool cuda_input = false, cuda_failed = false;
	std::unique_ptr<opaque_buffer_reader> opaque; // the other encoders: a readback of the Vulkan memory
	struct d3d11_state; // desktop frames on the GPU: video processor BGRA -> NV12/P010 into D3D11 encoder frames
	std::unique_ptr<d3d11_state> d3d;
	bool d3d_input = false, d3d_failed = false;
	bool rgb_input = false;       // BGRA D3D11 frames straight into NVENC (its own RGB -> YUV conversion)
	bool eight_bit_input = false; // 10-bit codec fed NV12 (NVENC "highbitdepth"): the video processor has no P010 output
	void setup_d3d11(const captured_frame &);
	const uint8_t * read_texture(const captured_frame &, int & stride); // fallback: the texture on the CPU
	// VR frames for AMF / Media Foundation: a GPU copy from the runtime's Vulkan memory into D3D11 frames (no CPU copy)
	std::unique_ptr<class opaque_buffer_copier> copier;
	AVBufferRef * copy_device = nullptr;
	AVBufferRef * copy_frames = nullptr;
	bool copy_input = false, copy_failed = false;
	void setup_vr_d3d11(AVPixelFormat sw_format);
#else
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
	bool cuda_input = false;
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
#endif
};

// Picks the render node of the GPU driving the displays (OPENDESK_RENDER_NODE overrides). Empty on Windows.
std::string default_render_node();
// The GPU for VR (apps render there, its buffers and encoder live there): the display GPU, except on hybrid laptops
// whose display runs on an integrated GPU, where the discrete one (OPENDESK_VR_RENDER_NODE overrides).
std::string vr_render_node();
unsigned gpu_vendor(const std::string & render_node);
// Linux: opens the VAAPI device and looks for the H.264 encoder; Windows: tries the available encoders.
// Empty string = usable, else the reason.
std::string hardware_encoder_probe(const std::string & render_node);
// The codecs the GPU can encode (one small frame each; takes a moment - call off the UI thread)
std::vector<video_codec> hardware_encoder_codecs(const std::string & render_node);
} // namespace od
