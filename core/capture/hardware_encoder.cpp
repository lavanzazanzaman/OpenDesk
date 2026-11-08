#include "hardware_encoder.h"
#include "dmabuf_readback.h"
#include "vk_buffers.h"

// hwcontext_cuda.h wants cuda.h only for these two handle types
#define CUDA_VERSION 12000
typedef struct CUctx_st * CUcontext;
typedef struct CUstream_st * CUstream;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <vulkan/vulkan.h>

namespace od
{
struct hardware_encoder::readback : dmabuf_readback { using dmabuf_readback::dmabuf_readback; };
namespace
{
void check(int err, const char * what)
{
	if (err < 0)
	{
		char buf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(err, buf, sizeof(buf));
		throw std::runtime_error(std::format("{}: {}", what, buf));
	}
}

int64_t now_us()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool connector_connected(const std::filesystem::path & card)
{
	std::error_code ec;
	for (const auto & e: std::filesystem::directory_iterator(card.parent_path(), ec))
	{
		std::string name = e.path().filename().string();
		if (!name.starts_with(card.filename().string() + "-"))
			continue;
		std::ifstream status(e.path() / "status");
		std::string s;
		if (status >> s && s == "connected")
			return true;
	}
	return false;
}
} // namespace

// The few CUDA driver API calls for importing Vulkan memory (cuda.h types, stable ABI), from libcuda at runtime
struct hardware_encoder::cuda_state
{
	using CUresult = int;
	using CUdeviceptr = unsigned long long;
	using CUexternalMemory = void *;
	struct handle_desc
	{
		int type; // CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD = 1
		union
		{
			int fd;
			struct
			{
				void * handle;
				const void * name;
			} win32;
			const void * sci;
		} handle;
		unsigned long long size;
		unsigned int flags; // CUDA_EXTERNAL_MEMORY_DEDICATED = 1
		unsigned int reserved[16];
	};
	struct buffer_desc
	{
		unsigned long long offset, size;
		unsigned int flags;
		unsigned int reserved[16];
	};
	static_assert(sizeof(handle_desc) == 104 && sizeof(buffer_desc) == 88);

	CUresult (*import_memory)(CUexternalMemory *, const handle_desc *) = nullptr;
	CUresult (*mapped_buffer)(CUdeviceptr *, CUexternalMemory, const buffer_desc *) = nullptr;
	CUresult (*destroy_memory)(CUexternalMemory) = nullptr;
	CUresult (*mem_free)(CUdeviceptr) = nullptr;
	CUresult (*push)(CUcontext) = nullptr;
	CUresult (*pop)(CUcontext *) = nullptr;

	AVBufferRef * device = nullptr;
	AVBufferRef * frames = nullptr;
	CUcontext ctx = nullptr;
	struct buffer
	{
		CUexternalMemory memory = nullptr;
		CUdeviceptr ptr = 0;
		AVBufferRef * ref = nullptr;
	};
	std::map<uint64_t, buffer> buffers; // by captured_frame::buffer_id

	cuda_state(int gpu, int width, int height, AVPixelFormat sw_format)
	{
		void * lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL); // stays loaded (FFmpeg uses it too)
		if (!lib)
			throw std::runtime_error("libcuda.so.1 unavailable");
		import_memory = reinterpret_cast<decltype(import_memory)>(dlsym(lib, "cuImportExternalMemory"));
		mapped_buffer = reinterpret_cast<decltype(mapped_buffer)>(dlsym(lib, "cuExternalMemoryGetMappedBuffer"));
		destroy_memory = reinterpret_cast<decltype(destroy_memory)>(dlsym(lib, "cuDestroyExternalMemory"));
		mem_free = reinterpret_cast<decltype(mem_free)>(dlsym(lib, "cuMemFree_v2"));
		push = reinterpret_cast<decltype(push)>(dlsym(lib, "cuCtxPushCurrent_v2"));
		pop = reinterpret_cast<decltype(pop)>(dlsym(lib, "cuCtxPopCurrent_v2"));
		if (!import_memory || !mapped_buffer || !destroy_memory || !mem_free || !push || !pop)
			throw std::runtime_error("libcuda lacks external memory support");
		check(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, std::to_string(gpu).c_str(), nullptr, 0), "CUDA device");
		ctx = static_cast<AVCUDADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(device->data)->hwctx)->cuda_ctx;
		frames = av_hwframe_ctx_alloc(device);
		auto * fc = reinterpret_cast<AVHWFramesContext *>(frames->data);
		fc->format = AV_PIX_FMT_CUDA;
		fc->sw_format = sw_format; // NV12 / P010 from the runtime
		fc->width = width;
		fc->height = height;
		check(av_hwframe_ctx_init(frames), "CUDA frames");
	}

	~cuda_state()
	{
		clear();
		av_buffer_unref(&frames);
		av_buffer_unref(&device);
	}

	void clear()
	{
		if (buffers.empty())
			return;
		push(ctx);
		for (auto & [id, b]: buffers)
		{
			av_buffer_unref(&b.ref);
			mem_free(b.ptr);
			destroy_memory(b.memory);
		}
		CUcontext popped;
		pop(&popped);
		buffers.clear();
	}

	const buffer & import(const captured_frame & f)
	{
		if (auto it = buffers.find(f.buffer_id); it != buffers.end())
			return it->second;
		push(ctx);
		struct pop_on_exit
		{
			cuda_state * s;
			~pop_on_exit()
			{
				CUcontext popped;
				s->pop(&popped);
			}
		} guard{this};
		handle_desc hd{};
		hd.type = 1;
		hd.handle.fd = dup(f.fd); // CUDA owns it on success
		hd.size = f.size;
		hd.flags = 1;
		buffer b;
		if (CUresult r = import_memory(&b.memory, &hd); r != 0)
		{
			close(hd.handle.fd);
			throw std::runtime_error(std::format("cuImportExternalMemory failed ({})", r));
		}
		buffer_desc bd{};
		bd.size = f.size;
		if (CUresult r = mapped_buffer(&b.ptr, b.memory, &bd); r != 0)
		{
			destroy_memory(b.memory);
			throw std::runtime_error(std::format("cuExternalMemoryGetMappedBuffer failed ({})", r));
		}
		b.ref = av_buffer_create(reinterpret_cast<uint8_t *>(b.ptr), f.size, [](void *, uint8_t *) {}, nullptr, 0);
		return buffers.emplace(f.buffer_id, b).first->second;
	}
};

unsigned gpu_vendor(const std::string & node)
{
	unsigned vendor = 0;
	std::ifstream in(std::filesystem::path("/sys/class/drm") / std::filesystem::path(node).filename() / "device/vendor");
	in >> std::hex >> vendor;
	return vendor;
}

// CUDA ordinals are not DRM render-node indices. Match by PCI address on multi-GPU hosts.
static int cuda_gpu(const std::string & node)
{
	void * lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!lib) throw std::runtime_error("NVIDIA driver unavailable (libcuda.so.1)");
	std::unique_ptr<void, int(*)(void*)> handle(lib, dlclose);
	auto init = reinterpret_cast<int(*)(unsigned)>(dlsym(lib, "cuInit"));
	auto count_fn = reinterpret_cast<int(*)(int*)>(dlsym(lib, "cuDeviceGetCount"));
	auto pci_fn = reinterpret_cast<int(*)(char*, int, int)>(dlsym(lib, "cuDeviceGetPCIBusId"));
	int count = 0;
	if (!init || !count_fn || !pci_fn || init(0) || count_fn(&count)) throw std::runtime_error("NVIDIA CUDA driver initialization failed");
	std::error_code ec;
	std::string pci = std::filesystem::canonical(std::filesystem::path("/sys/class/drm") / std::filesystem::path(node).filename() / "device", ec).filename();
	unsigned domain, bus, device, function;
	if (std::sscanf(pci.c_str(), "%x:%x:%x.%x", &domain, &bus, &device, &function) != 4) throw std::runtime_error("Cannot identify NVIDIA PCI device for " + node);
	for (int i = 0; i < count; ++i) {
		char address[32]; unsigned d, b, v, f;
		if (!pci_fn(address, sizeof(address), i) && std::sscanf(address, "%x:%x:%x.%x", &d, &b, &v, &f) == 4 && d == domain && b == bus && v == device && f == function) return i;
	}
	throw std::runtime_error("No CUDA device matches " + node);
}

std::string default_render_node()
{
	if (const char * env = std::getenv("OPENDESK_RENDER_NODE"))
		return env;
	// the render node sharing a PCI device with a card that has a connected display
	std::error_code ec;
	for (const auto & e: std::filesystem::directory_iterator("/sys/class/drm", ec))
	{
		std::string name = e.path().filename().string();
		if (!name.starts_with("card") || name.find('-') != std::string::npos || !connector_connected(e.path()))
			continue;
		for (const auto & r: std::filesystem::directory_iterator(e.path() / "device/drm", ec))
			if (r.path().filename().string().starts_with("renderD"))
				return "/dev/dri/" + r.path().filename().string();
	}
	return "/dev/dri/renderD128";
}

namespace
{
std::mutex selection_mutex;
std::string selected_game, selected_streaming; // PCI addresses

std::string node_of_pci(const std::string & pci)
{
	if (pci.empty())
		return {};
	for (const gpu_info & g: list_gpus())
		if (g.pci == pci)
			return g.node;
	return {}; // that card is gone: automatic
}
} // namespace

void set_gpu_selection(const std::string & game_pci, const std::string & streaming_pci)
{
	std::lock_guard lock(selection_mutex);
	selected_game = game_pci;
	selected_streaming = streaming_pci;
}

std::vector<gpu_info> list_gpus()
{
	static const std::vector<gpu_info> gpus = [] { // (asked often; cards don't come and go while running)
	std::vector<gpu_info> out;
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance instance;
	if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
		return out;
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
		if (std::none_of(ext.begin(), ext.end(), [](auto & e) { return !std::strcmp(e.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME); }))
			continue;
		VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
		VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drm};
		vkGetPhysicalDeviceProperties2(d, &props);
		if (!drm.hasRender || props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
			continue;
		gpu_info g;
		g.node = "/dev/dri/renderD" + std::to_string(drm.renderMinor);
		g.name = props.properties.deviceName;
		if (const auto paren = g.name.find(" ("); paren != std::string::npos)
			g.name.resize(paren); // "AMD Radeon RX 9070 XT (RADV GFX1201)"
		g.vendor = gpu_vendor(g.node);
		std::error_code ec;
		g.pci = std::filesystem::canonical(std::filesystem::path("/sys/class/drm") / std::filesystem::path(g.node).filename() / "device", ec).filename().string();
		out.push_back(std::move(g));
	}
	vkDestroyInstance(instance, nullptr);
	return out;
	}();
	return gpus;
}

std::string vr_encode_node()
{
	if (const char * env = std::getenv("OPENDESK_VR_ENCODE_NODE"))
		return env;
	std::string pci;
	{
		std::lock_guard lock(selection_mutex);
		pci = selected_streaming;
	}
	const std::string node = node_of_pci(pci);
	return node.empty() ? vr_render_node() : node;
}

std::string desktop_encode_node()
{
	// The display's GPU encodes straight from its own DMA-BUFs. Another card would need a readback and a CPU colour
	// conversion per frame: a 3440x1440 desktop then reached only about 38 fps.
	return default_render_node();
}

std::string vr_render_node()
{
	if (const char * env = std::getenv("OPENDESK_VR_RENDER_NODE"))
		return env;
	{
		std::string pci;
		{
			std::lock_guard lock(selection_mutex);
			pci = selected_game;
		}
		if (const std::string node = node_of_pci(pci); !node.empty())
			return node;
	}
	return auto_vr_render_node();
}

std::string auto_vr_render_node()
{
	static const std::string chosen = [] {
		const std::string display = default_render_node();
		struct stat st{};
		if (stat(display.c_str(), &st) != 0)
			return display;
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &app;
		VkInstance instance;
		if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
			return display;
		uint32_t n = 0;
		vkEnumeratePhysicalDevices(instance, &n, nullptr);
		std::vector<VkPhysicalDevice> devices(n);
		vkEnumeratePhysicalDevices(instance, &n, devices.data());
		bool display_integrated = false;
		std::string discrete;
		for (VkPhysicalDevice d: devices)
		{
			uint32_t count = 0;
			vkEnumerateDeviceExtensionProperties(d, nullptr, &count, nullptr);
			std::vector<VkExtensionProperties> ext(count);
			vkEnumerateDeviceExtensionProperties(d, nullptr, &count, ext.data());
			if (std::none_of(ext.begin(), ext.end(), [](auto & e) { return !std::strcmp(e.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME); }))
				continue;
			VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
			VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drm};
			vkGetPhysicalDeviceProperties2(d, &props);
			if (!drm.hasRender)
				continue;
			const auto type = props.properties.deviceType;
			if (uint32_t(drm.renderMajor) == major(st.st_rdev) && uint32_t(drm.renderMinor) == minor(st.st_rdev))
				display_integrated = type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
			else if (type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && discrete.empty())
				discrete = "/dev/dri/renderD" + std::to_string(drm.renderMinor);
		}
		vkDestroyInstance(instance, nullptr);
		return display_integrated && !discrete.empty() ? discrete : display;
	}();
	return chosen;
}

std::string hardware_encoder_probe(const std::string & render_node)
{
	if (gpu_vendor(render_node) == 0x10de) {
		try {
			// NVENC's smallest H.264 frame is ~145x49: anything below fails InitializeEncoder ("Invalid argument")
			hardware_encoder enc({256, 256, 60, 1000, video_codec::h264, render_node});
			std::vector<uint8_t> pixels(256 * 256 * 4, 0);
			captured_frame f; f.data = pixels.data(); f.stride = 1024; f.av_sw_format = AV_PIX_FMT_BGR0;
			bool got = false; enc.encode(f, true, [&](bytes &&, bool) { got = true; });
			return got ? "" : "NVENC produced no frame";
		} catch (const std::exception & e) { return e.what(); }
	}
	if (!avcodec_find_encoder_by_name("h264_vaapi"))
		return "FFmpeg was built without VAAPI encoders";
	AVBufferRef * dev = nullptr;
	int err = av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VAAPI, render_node.c_str(), nullptr, 0);
	av_buffer_unref(&dev);
	if (err < 0)
	{
		char buf[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(err, buf, sizeof(buf));
		return std::format("cannot open VAAPI on {}: {} (install the VA-API driver, e.g. mesa / intel-media-driver)", render_node, buf);
	}
	return {};
}

std::vector<video_codec> hardware_encoder_codecs(const std::string & render_node)
{
	std::vector<video_codec> out;
	for (video_codec c: {video_codec::h264, video_codec::hevc, video_codec::hevc_10bit, video_codec::av1, video_codec::av1_10bit})
	{
		try
		{
			// a real stream size: some encoders reject tiny frames for HEVC/AV1 (AMD: "Invalid argument" at 256x256)
			constexpr int w = 1280, h = 720;
			hardware_encoder enc({w, h, 60, 5000, c, render_node});
			std::vector<uint8_t> pixels(w * h * 4, 0);
			captured_frame f;
			f.data = pixels.data();
			f.stride = w * 4;
			f.av_sw_format = AV_PIX_FMT_BGR0;
			bool got = false;
			for (int i = 0; i < 8 && !got; ++i) // HEVC/AV1 on VAAPI hand out their first packet a few frames later
				enc.encode(f, i == 0, [&](bytes &&, bool) { got = true; });
			if (got)
				out.push_back(c);
			else if (getenv("OPENDESK_DEBUG_CODEC_PROBE"))
				fprintf(stderr, "codec probe %s: no packet\n", codec_name(c));
		}
		catch (const std::exception & e)
		{
			if (getenv("OPENDESK_DEBUG_CODEC_PROBE"))
				fprintf(stderr, "codec probe %s: %s\n", codec_name(c), e.what());
		}
	}
	return out;
}

hardware_encoder::hardware_encoder(encoder_config c) :
        cfg(std::move(c))
{
	if (cfg.render_node.empty()) cfg.render_node = default_render_node();
	nvenc = gpu_vendor(cfg.render_node) == 0x10de;
	if (nvenc) return;
	// DRM device on the render node (DMA-BUFs are wrapped as DRM-PRIME frames), VAAPI derived from it
	// (FFmpeg can derive VAAPI from DRM, not the reverse)
	check(av_hwdevice_ctx_create(&drm_device, AV_HWDEVICE_TYPE_DRM, cfg.render_node.c_str(), nullptr, 0), "DRM device");
	check(av_hwdevice_ctx_create_derived(&vaapi_device, AV_HWDEVICE_TYPE_VAAPI, drm_device, 0), "VAAPI device");
}

hardware_encoder::~hardware_encoder()
{
	sws_freeContext(sws);
	av_frame_free(&last_nv12);
	avcodec_free_context(&codec); // unregisters NVENC's CUDA inputs before they are freed
	cuda.reset();
	avfilter_graph_free(&graph);
	av_buffer_unref(&drm_frames);
	av_buffer_unref(&drm_device);
	av_buffer_unref(&vaapi_device);
}

void hardware_encoder::setup_filters(const captured_frame & f)
{
	if (nvenc) { setup_nvenc(f); return; }
	const bool dmabuf = f.fd >= 0 && !f.opaque_buffer; // opaque buffers arrive as CPU copies (host_frame)
	AVPixelFormat sw_format = dmabuf ? (f.drm_format == 0x34325241 /* AR24 */ ? AV_PIX_FMT_BGRA : AV_PIX_FMT_BGR0)
	                                 : AVPixelFormat(f.av_sw_format);
	if (graph && graph_is_dmabuf == dmabuf && graph_sw_format == sw_format)
		return;
	avfilter_graph_free(&graph);
	av_buffer_unref(&drm_frames);
	graph = avfilter_graph_alloc();

	AVBufferSrcParameters * par = av_buffersrc_parameters_alloc();
	par->width = cfg.width;
	par->height = cfg.height;
	par->time_base = {1, 1000000};
	if (dmabuf)
	{
		drm_frames = av_hwframe_ctx_alloc(drm_device);
		auto * fc = reinterpret_cast<AVHWFramesContext *>(drm_frames->data);
		fc->format = AV_PIX_FMT_DRM_PRIME;
		fc->sw_format = sw_format;
		fc->width = cfg.width;
		fc->height = cfg.height;
		check(av_hwframe_ctx_init(drm_frames), "DRM frames context");
		par->format = AV_PIX_FMT_DRM_PRIME;
		par->hw_frames_ctx = drm_frames;
	}
	else
		par->format = sw_format;

	src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "in");
	check(av_buffersrc_parameters_set(src, par), "buffersrc parameters");
	av_free(par);
	check(avfilter_init_str(src, nullptr), "buffersrc init");
	check(avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph), "buffersink");

	// GPU colour conversion: BT.709, limited or full range, 8-bit NV12 or 10-bit P010
	std::string convert = std::format("scale_vaapi=format={}:out_color_matrix=bt709:out_range={}",
	                                  is_10bit(cfg.codec) ? "p010" : "nv12", cfg.full_range ? "full" : "limited");
	// YUV the runtime already converted (NV12/P010 Vulkan buffers): upload only
	const bool yuv_ready = !dmabuf && sw_format == (is_10bit(cfg.codec) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12);
	std::string chain_str = yuv_ready ? std::string("hwupload") : (dmabuf ? "hwmap=derive_device=vaapi," : "hwupload,") + convert;
	const char * chain = chain_str.c_str();
	// segment API (like the ffmpeg CLI): filters get their device *before* init, which hwupload needs.
	// segment_apply returns the chain's open pads, which we link to buffersrc / buffersink ourselves.
	AVFilterGraphSegment * seg = nullptr;
	AVFilterInOut * open_in = nullptr;
	AVFilterInOut * open_out = nullptr;
	int err = avfilter_graph_segment_parse(graph, chain, 0, &seg);
	if (err >= 0)
		err = avfilter_graph_segment_create_filters(seg, 0);
	if (err >= 0)
		for (size_t c = 0; c < seg->nb_chains; ++c)
			for (size_t f = 0; f < seg->chains[c]->nb_filters; ++f)
				if (AVFilterContext * fc = seg->chains[c]->filters[f]->filter)
					fc->hw_device_ctx = av_buffer_ref(vaapi_device);
	if (err >= 0)
		err = avfilter_graph_segment_apply(seg, 0, &open_in, &open_out);
	if (err >= 0 && (!open_in || !open_out))
		err = AVERROR(EINVAL);
	if (err >= 0)
		err = avfilter_link(src, 0, open_in->filter_ctx, unsigned(open_in->pad_idx));
	if (err >= 0)
		err = avfilter_link(open_out->filter_ctx, unsigned(open_out->pad_idx), sink, 0);
	avfilter_graph_segment_free(&seg);
	avfilter_inout_free(&open_in);
	avfilter_inout_free(&open_out);
	check(err, "filter graph");
	check(avfilter_graph_config(graph, nullptr), "filter graph config");
	graph_is_dmabuf = dmabuf;
	graph_sw_format = sw_format;

	if (!codec) // the encoder takes its surfaces from scale_vaapi's output pool
	{
		const char * name = is_hevc(cfg.codec) ? "hevc_vaapi" : is_av1(cfg.codec) ? "av1_vaapi" : "h264_vaapi";
		const AVCodec * enc = avcodec_find_encoder_by_name(name);
		if (!enc)
			throw std::runtime_error(std::format("{} not available in this FFmpeg", name));
		codec = avcodec_alloc_context3(enc);
		codec->width = cfg.width;
		codec->height = cfg.height;
		codec->time_base = {1, 1000000};
		codec->framerate = {cfg.fps, 1};
		codec->pix_fmt = AV_PIX_FMT_VAAPI;
		codec->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(sink));
		codec->bit_rate = int64_t(cfg.kbps) * 1000;
		codec->rc_max_rate = codec->bit_rate;
		codec->rc_buffer_size = int(codec->bit_rate / cfg.fps * 2); // ~2 frames: low latency, no big bursts
		codec->gop_size = cfg.fps * 10;                              // IDR on demand (ForceKeyFrame) + safety net
		codec->max_b_frames = 0;
		codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
		// the headset reads full/limited range from the bitstream (MediaCodec color-range)
		codec->color_range = cfg.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		codec->colorspace = AVCOL_SPC_BT709;
		codec->color_primaries = AVCOL_PRI_BT709;
		codec->color_trc = AVCOL_TRC_BT709;
		av_opt_set(codec->priv_data, "rc_mode", "CBR", 0);
		// Mesa radeonsi VA quality level (frontends/va/encode.c, vlVaQualityBits):
		//   bit0 valid, bits1-2 preset (0 speed), bit3 pre-encode, bit4 VBAQ; 0 = speed preset, nothing extra
		if (gpu_vendor(cfg.render_node) == 0x1002 && (cfg.pre_encode || cfg.vbaq || cfg.amd_preset))
			codec->compression_level = 1 | ((cfg.amd_preset & 3) << 1) | (cfg.pre_encode ? 1 << 3 : 0) | (cfg.vbaq ? 1 << 4 : 0);
		av_opt_set_int(codec->priv_data, "async_depth", 1, 0); // no pipelined extra frame
		if (cfg.codec == video_codec::h264 || cfg.codec == video_codec::h264_plus)
			av_opt_set(codec->priv_data, "profile", "high", 0);
		else if (cfg.codec == video_codec::hevc_10bit)
			av_opt_set(codec->priv_data, "profile", "main10", 0);
		check(avcodec_open2(codec, enc, nullptr), "open encoder");
	}
}

void hardware_encoder::encode(const captured_frame & f, bool force_idr, const au_callback & on_au)
{
	stage(f);
	reencode_last(force_idr, on_au);
}

void hardware_encoder::note(const std::string & line)
{
	if (log)
		log(line);
	else
		fprintf(stderr, "%s\n", line.c_str());
}

std::string hardware_encoder::input_path() const
{
	return std::string(backend()) + (cuda_input ? ", CUDA input" : cuda_upload ? ", CUDA upload" : cpu_copy ? ", CPU copy" : "");
}

void hardware_encoder::prime(const captured_frame & f)
{
	cpu_copy = f.opaque_buffer || (nvenc && f.fd >= 0);
	setup_filters(f);
	if (!cuda_input && !cuda_upload)
		return;
	cpu_copy = false;
	// one trial encode through CUDA (the output is dropped; the stream starts with an IDR anyway)
	try
	{
		stage(f);
		reencode_last(true, [](bytes &&, bool) {});
	}
	catch (const std::exception & e)
	{
		note(std::string("[encoder] NVENC from CUDA failed (") + e.what() + "): copying through the CPU");
		av_frame_free(&last_nv12);
		avcodec_free_context(&codec);
		cuda.reset();
		cuda_input = cuda_upload = false;
		cuda_failed = true;
		pts_origin = last_pts = -1;
		setup_filters(f);
	}
}

const captured_frame & hardware_encoder::host_frame(const captured_frame & f, captured_frame & storage)
{
	if (!f.opaque_buffer)
		return f;
	if (!opaque)
		opaque = std::make_unique<opaque_buffer_reader>(cfg.source_node.empty() ? cfg.render_node : cfg.source_node);
	storage = f;
	storage.data = opaque->read(f.fd, f.size, f.buffer_bytes, f.buffer_id, f.host_visible);
	storage.fd = -1;
	storage.opaque_buffer = false;
	return storage;
}

void hardware_encoder::stage(const captured_frame & frame)
{
	if (nvenc) { stage_nvenc(frame); return; }
	captured_frame storage;
	const captured_frame & f = host_frame(frame, storage);
	setup_filters(f);
	AVFrame * in = av_frame_alloc();
	in->width = cfg.width;
	in->height = cfg.height;
	int64_t t = now_us();
	if (pts_origin < 0)
		pts_origin = t;
	in->pts = std::max(t - pts_origin, last_pts + 1);
	last_pts = in->pts;

	if (f.fd >= 0)
	{
		// wrap the DMA-BUF: the descriptor is freed with the frame, the fd stays owned by PipeWire
		auto * desc = static_cast<AVDRMFrameDescriptor *>(av_mallocz(sizeof(AVDRMFrameDescriptor)));
		desc->nb_objects = 1;
		desc->objects[0].fd = f.fd;
		desc->objects[0].size = f.size ? f.size : size_t(f.offset) + size_t(f.stride) * size_t(cfg.height);
		desc->objects[0].format_modifier = f.modifier;
		desc->nb_layers = 1;
		desc->layers[0].format = f.drm_format;
		desc->layers[0].nb_planes = 1;
		desc->layers[0].planes[0] = {0, ptrdiff_t(f.offset), ptrdiff_t(f.stride)};
		in->format = AV_PIX_FMT_DRM_PRIME;
		in->data[0] = reinterpret_cast<uint8_t *>(desc);
		in->buf[0] = av_buffer_create(reinterpret_cast<uint8_t *>(desc), sizeof(*desc),
		                              [](void *, uint8_t * d) { av_free(d); }, nullptr, 0);
		in->hw_frames_ctx = av_buffer_ref(drm_frames);
	}
	else
	{
		in->format = graph_sw_format;
		in->data[0] = const_cast<uint8_t *>(f.data);
		in->linesize[0] = int(f.stride);
		if (f.uv_offset) // NV12 / P010
		{
			in->data[1] = const_cast<uint8_t *>(f.data) + f.uv_offset;
			in->linesize[1] = int(f.stride);
		}
	}

	// KEEP_REF: buffersrc must not keep pointers into PipeWire memory after we return
	check(av_buffersrc_add_frame_flags(src, in, AV_BUFFERSRC_FLAG_KEEP_REF | AV_BUFFERSRC_FLAG_PUSH), "filter input");
	av_frame_free(&in);
	AVFrame * nv12 = av_frame_alloc();
	while (av_buffersink_get_frame(sink, nv12) >= 0) // newest wins
	{
		av_frame_free(&last_nv12);
		last_nv12 = av_frame_clone(nv12);
		av_frame_unref(nv12);
	}
	av_frame_free(&nv12);
}

void hardware_encoder::setup_nvenc(const captured_frame & f)
{
	if (codec) return;
	// Vulkan buffers (NV12/P010 from the runtime) go to NVENC through CUDA without a copy (WiVRn's way), when their
	// bit depth is the codec's
	cuda_input = cuda_upload = false;
	const auto yuv = AVPixelFormat(f.av_sw_format);
	if (f.opaque_buffer && !cuda_failed && yuv == (is_10bit(cfg.codec) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12))
		try
		{
			cuda = std::make_unique<cuda_state>(cuda_gpu(cfg.render_node), cfg.width, cfg.height, yuv);
			if (!cfg.source_node.empty() && cfg.source_node != cfg.render_node)
				cuda_upload = true; // the frames are on another GPU: uploaded by DMA instead of imported
			else
				cuda_input = true;
		}
		catch (const std::exception & e)
		{
			note(std::string("[encoder] CUDA input unavailable (") + e.what() + "): copying through the CPU");
		}
	const char * name = is_av1(cfg.codec) ? "av1_nvenc" : is_hevc(cfg.codec) ? "hevc_nvenc" : "h264_nvenc";
	const AVCodec * enc = avcodec_find_encoder_by_name(name);
	if (!enc) throw std::runtime_error(std::string(name) + " is missing from FFmpeg");
	codec = avcodec_alloc_context3(enc);
	codec->width = cfg.width; codec->height = cfg.height;
	codec->time_base = {1, 1000000}; codec->framerate = {cfg.fps, 1};
	codec->pix_fmt = cuda_input || cuda_upload ? AV_PIX_FMT_CUDA : is_10bit(cfg.codec) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
	if (cuda_input || cuda_upload)
		codec->hw_frames_ctx = av_buffer_ref(cuda->frames);
	codec->bit_rate = int64_t(cfg.kbps) * 1000;
	codec->rc_max_rate = codec->bit_rate;
	codec->rc_buffer_size = int(codec->bit_rate / std::max(cfg.fps, 1) * 2);
	codec->gop_size = cfg.fps * 10; codec->max_b_frames = 0;
	codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	// no periodic keyframes (WiVRn's way): intra refresh rebuilds the picture over a second of frames instead, so no
	// frame is several times larger than the rest (decode spikes on the headset). IDRs still come on request.
	const bool intra_refresh = av_opt_find(codec->priv_data, "intra-refresh", nullptr, 0, 0) != nullptr;
	if (intra_refresh)
		codec->gop_size = cfg.fps; // FFmpeg: refresh period; the GOP itself becomes infinite
	codec->color_range = cfg.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
	codec->colorspace = AVCOL_SPC_BT709;
	codec->color_primaries = AVCOL_PRI_BT709; codec->color_trc = AVCOL_TRC_BT709;
	auto opt = [&](const char * key, const char * value) { check(av_opt_set(codec->priv_data, key, value, 0), key); };
	opt("preset", std::format("p{}", std::clamp(cfg.preset, 1, 7)).c_str()); opt("tune", "ull"); opt("rc", "cbr");
	// "2-pass": the analysis pass at quarter resolution - most of the rate-control gain for a fraction of the cost
	// (full resolution nearly doubled NVENC's time per VR eye on a laptop RTX 4060)
	opt("multipass", cfg.pre_encode ? "qres" : "disabled");
	opt("spatial-aq", cfg.vbaq ? "1" : "0");
	opt("rc-lookahead", "0"); opt("zerolatency", "1"); opt("delay", "0"); opt("forced-idr", "1");
	if (intra_refresh)
		opt("intra-refresh", "1");
	if (!cuda_input && !cuda_upload) // with CUDA frames the frames' device decides
		check(av_opt_set_int(codec->priv_data, "gpu", cuda_gpu(cfg.render_node), 0), "NVENC GPU");
	check(avcodec_open2(codec, enc, nullptr), "open NVENC encoder");
	note(std::format("[encoder] {} on {}, preset p{}, multipass={}, spatial-aq={}{}{}", name, cfg.render_node, std::clamp(cfg.preset, 1, 7),
	                 cfg.pre_encode ? "qres" : "disabled", int(cfg.vbaq), intra_refresh ? ", intra refresh" : "",
	                 cuda_input ? ", CUDA input" : cuda_upload ? ", CUDA upload" : ""));
}

void hardware_encoder::stage_nvenc(const captured_frame & frame)
{
	if (cuda_input && cuda && !cuda->buffers.count(frame.buffer_id) && cuda->buffers.size() >= 12)
	{
		// the buffer pool was replaced: start over (NVENC keeps the old ones registered until it closes)
		av_frame_free(&last_nv12);
		avcodec_free_context(&codec);
		cuda.reset();
	}
	setup_nvenc(frame);
	if (cuda_input)
	{
		const auto & b = cuda->import(frame);
		AVFrame * in = av_frame_alloc();
		in->format = AV_PIX_FMT_CUDA;
		in->width = cfg.width;
		in->height = cfg.height;
		in->hw_frames_ctx = av_buffer_ref(cuda->frames);
		in->buf[0] = av_buffer_ref(b.ref);
		in->data[0] = reinterpret_cast<uint8_t *>(b.ptr);
		in->linesize[0] = int(frame.stride);
		in->data[1] = reinterpret_cast<uint8_t *>(b.ptr + frame.uv_offset); // NVENC: chroma right after the luma rows
		in->linesize[1] = int(frame.stride);
		av_frame_free(&last_nv12);
		last_nv12 = in;
		if (pts_origin < 0) pts_origin = now_us();
		return;
	}
	captured_frame storage;
	const captured_frame & f = host_frame(frame, storage);
	const uint8_t * pixels = f.data;
	AVPixelFormat format = AVPixelFormat(f.av_sw_format);
	int stride = int(f.stride);
	if (f.fd >= 0) {
		if (!download) download = std::make_unique<readback>(cfg.source_node.empty() ? cfg.render_node : cfg.source_node);
		pixels = download->read(f, cfg.width, cfg.height);
		format = AV_PIX_FMT_RGBA; stride = cfg.width * 4;
	}
	if (!pixels || stride <= 0) throw std::runtime_error("NVENC: invalid input frame");
	if (cuda_upload)
	{
		// into CUDA memory by DMA: no CPU copy over the bus, and CUDA keeps the card out of its low power states
		AVFrame * src = av_frame_alloc();
		AVFrame * dst = av_frame_alloc();
		src->format = format;
		src->width = cfg.width;
		src->height = cfg.height;
		src->data[0] = const_cast<uint8_t *>(pixels);
		src->linesize[0] = stride;
		src->data[1] = const_cast<uint8_t *>(pixels + f.uv_offset);
		src->linesize[1] = stride;
		dst->format = AV_PIX_FMT_CUDA;
		dst->width = cfg.width;
		dst->height = cfg.height;
		int err = av_hwframe_get_buffer(cuda->frames, dst, 0);
		if (err >= 0)
			err = av_hwframe_transfer_data(dst, src, 0);
		av_frame_free(&src);
		if (err < 0)
		{
			av_frame_free(&dst);
			check(err, "CUDA upload");
		}
		av_frame_free(&last_nv12);
		last_nv12 = dst;
		if (pts_origin < 0) pts_origin = now_us();
		return;
	}
	if (f.uv_offset && format == codec->pix_fmt)
	{
		// already the encoder's layout (the runtime's NV12/P010): FFmpeg's upload is the only copy. Valid until the next
		// frame: VR frames are encoded right here, before their buffer goes back to the compositor
		AVFrame * in = av_frame_alloc();
		in->format = codec->pix_fmt;
		in->width = cfg.width;
		in->height = cfg.height;
		in->buf[0] = av_buffer_create(const_cast<uint8_t *>(pixels), f.size, [](void *, uint8_t *) {}, nullptr, AV_BUFFER_FLAG_READONLY);
		in->data[0] = const_cast<uint8_t *>(pixels);
		in->linesize[0] = stride;
		in->data[1] = const_cast<uint8_t *>(pixels + f.uv_offset);
		in->linesize[1] = stride;
		av_frame_free(&last_nv12);
		last_nv12 = in;
		if (pts_origin < 0) pts_origin = now_us();
		return;
	}
	sws = sws_getCachedContext(sws, cfg.width, cfg.height, format, cfg.width, cfg.height, codec->pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws) throw std::runtime_error("NVENC: RGB conversion unavailable");
	const int * coefficients = sws_getCoefficients(SWS_CS_ITU709);
	// RGB input is full range; NV12/P010 from the runtime already has the stream's range
	check(sws_setColorspaceDetails(sws, coefficients, f.uv_offset ? cfg.full_range : 1, coefficients, cfg.full_range, 0, 1 << 16, 1 << 16),
	      "NVENC color range");
	if (!last_nv12) {
		last_nv12 = av_frame_alloc(); last_nv12->format = codec->pix_fmt;
		last_nv12->width = cfg.width; last_nv12->height = cfg.height;
		check(av_frame_get_buffer(last_nv12, 32), "NVENC input buffer");
	}
	check(av_frame_make_writable(last_nv12), "NVENC input writable");
	const uint8_t * data[4] = {pixels}; int strides[4] = {stride};
	if (f.uv_offset) { data[1] = pixels + f.uv_offset; strides[1] = stride; } // NV12 / P010 Vulkan buffers
	check(sws_scale(sws, data, strides, 0, cfg.height, last_nv12->data, last_nv12->linesize), "NVENC RGB conversion");
	if (pts_origin < 0) pts_origin = now_us();
}

bool hardware_encoder::reencode_last(bool force_idr, const au_callback & on_au)
{
	if (!last_nv12 || !codec)
		return false;
	AVFrame * f = av_frame_clone(last_nv12);
	f->pts = std::max(now_us() - pts_origin, last_pts + 1);
	last_pts = f->pts;
	encode_nv12(f, force_idr, on_au);
	av_frame_free(&f);
	return true;
}

void hardware_encoder::encode_nv12(AVFrame * frame, bool force_idr, const au_callback & on_au)
{
	frame->pict_type = force_idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE; // I on vaapi = IDR
	if (force_idr)
		frame->flags |= AV_FRAME_FLAG_KEY;
	check(avcodec_send_frame(codec, frame), "encode");
	AVPacket * pkt = av_packet_alloc();
	while (avcodec_receive_packet(codec, pkt) >= 0)
	{
		bytes au;
		// AV1 "low overhead" stream for MediaCodec: each temporal unit starts with a temporal delimiter OBU
		if (is_av1(cfg.codec) && pkt->size > 0 && ((pkt->data[0] >> 3) & 0x0f) != 2)
			au = {0x12, 0x00};
		au.insert(au.end(), pkt->data, pkt->data + pkt->size);
		on_au(std::move(au), pkt->flags & AV_PKT_FLAG_KEY);
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);
}
} // namespace od

