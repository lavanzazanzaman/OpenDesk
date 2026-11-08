// Windows encoder. Inputs:
//   - desktop: BGRA frames in memory (DXGI Desktop Duplication) -> swscale NV12/P010;
//   - VR: NV12/P010 frames in exportable Vulkan memory (OPAQUE_WIN32 NT handles, vk_buffers.h) from the runtime.
// Encoders, on the GPU cfg.render_node names (win_gpu.h): NVENC, AMF, QSV, then Media Foundation and libx264/libx265.
// NVENC encodes VR frames in place through CUDA (cuImportExternalMemory, like the Linux NVENC path); every other encoder
// gets a GPU readback of them. One access unit per packet, no B-frames, no lookahead: the headset presents each frame as
// soon as it arrives.
#include "hardware_encoder.h"
#include "vk_buffers.h"
#include "win_gpu.h"

// hwcontext_cuda.h wants cuda.h only for these two handle types
#define CUDA_VERSION 12000
typedef struct CUctx_st * CUcontext;
typedef struct CUstream_st * CUstream;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace od
{
namespace
{
struct encoder_candidate
{
	const char * name;
	const char * backend;
	bool hardware;
};

// Hardware first. Names that FFmpeg was not built with are skipped by avcodec_find_encoder_by_name.
std::vector<encoder_candidate> candidates(video_codec c)
{
	switch (c)
	{
	case video_codec::h264:
	case video_codec::h264_plus:
		return {{"h264_nvenc", "NVENC", true},
		        {"h264_amf", "AMF", true},
		        {"h264_qsv", "QSV", true},
		        {"h264_mf", "Media Foundation", true},
		        {"libx264", "x264", false}};
	case video_codec::hevc:
	case video_codec::hevc_10bit:
		return {{"hevc_nvenc", "NVENC", true},
		        {"hevc_amf", "AMF", true},
		        {"hevc_qsv", "QSV", true},
		        {"hevc_mf", "Media Foundation", true},
		        {"libx265", "x265", false}};
	case video_codec::av1:
	case video_codec::av1_10bit:
		return {{"av1_nvenc", "NVENC", true}, {"av1_amf", "AMF", true}, {"av1_qsv", "QSV", true}};
	}
	return {};
}

// the vendor's own encoder first, so a hybrid laptop doesn't try NVENC on its Intel/AMD iGPU (and the other way round)
std::vector<encoder_candidate> ordered_candidates(video_codec c, unsigned vendor)
{
	auto list = candidates(c);
	const char * own = vendor == 0x10de ? "NVENC" : vendor == 0x1002 ? "AMF" : vendor == 0x8086 ? "QSV" : nullptr;
	if (own)
		std::stable_partition(list.begin(), list.end(), [&](const encoder_candidate & e) { return std::strcmp(e.backend, own) == 0; });
	return list;
}

void set_option(AVCodecContext * ctx, const char * name, const char * value)
{
	av_opt_set(ctx->priv_data, name, value, 0); // options a backend does not know are ignored
}

std::string av_error(int err)
{
	char buf[AV_ERROR_MAX_STRING_SIZE]{};
	av_strerror(err, buf, sizeof(buf));
	return buf;
}

void check(int err, const char * what)
{
	if (err < 0)
		throw std::runtime_error(std::format("{}: {}", what, av_error(err)));
}

void tune(AVCodecContext * ctx, const char * encoder, const encoder_config & cfg)
{
	if (std::strstr(encoder, "nvenc"))
	{
		set_option(ctx, "preset", std::format("p{}", std::clamp(cfg.preset, 1, 7)).c_str());
		set_option(ctx, "tune", "ull");
		set_option(ctx, "rc", "cbr");
		set_option(ctx, "zerolatency", "1");
		set_option(ctx, "delay", "0");
		set_option(ctx, "rc-lookahead", "0");
		set_option(ctx, "forced-idr", "1");
		set_option(ctx, "surfaces", "4");
		// "2-Pass encoding": NVENC's pre-analysis pass at quarter resolution gives most of the rate-control
		// gain for a fraction of the GPU time (same mapping as the Linux NVENC path).
		set_option(ctx, "multipass", cfg.pre_encode ? "qres" : "disabled");
		set_option(ctx, "spatial-aq", cfg.vbaq ? "1" : "0");
		// like Linux: no periodic keyframes; intra refresh rebuilds the picture over a second instead (no decode spikes
		// on the headset), IDRs still come on request
		if (av_opt_find(ctx->priv_data, "intra-refresh", nullptr, 0, 0))
		{
			set_option(ctx, "intra-refresh", "1");
			ctx->gop_size = std::max(cfg.fps, 1);
		}
	}
	else if (std::strstr(encoder, "amf"))
	{
		set_option(ctx, "usage", "ultralowlatency");
		set_option(ctx, "quality", "speed");
		set_option(ctx, "rc", "cbr");
		set_option(ctx, "enforce_hrd", "1");
		// "2-Pass encoding" -> AMF pre-analysis, "Adaptive quantization" -> VBAQ (the Linux VAAPI path sets the same
		// two through Mesa's quality-level bits)
		set_option(ctx, "preanalysis", cfg.pre_encode ? "1" : "0");
		set_option(ctx, "vbaq", cfg.vbaq ? "1" : "0");
	}
	else if (std::strstr(encoder, "qsv"))
	{
		set_option(ctx, "preset", "veryfast");
		set_option(ctx, "low_delay_brc", "1");
		set_option(ctx, "async_depth", "1");
	}
	else if (std::strstr(encoder, "mf"))
	{
		set_option(ctx, "scenario", "display_remoting");
		set_option(ctx, "hw_encoding", "1");
	}
	else if (std::strcmp(encoder, "libx264") == 0)
	{
		set_option(ctx, "preset", "ultrafast");
		set_option(ctx, "tune", "zerolatency");
	}
	else if (std::strcmp(encoder, "libx265") == 0)
	{
		set_option(ctx, "preset", "ultrafast");
		set_option(ctx, "tune", "zerolatency");
		set_option(ctx, "x265-params", "log-level=error:bframes=0");
	}
}

// FFmpeg's D3D11VA device string is the adapter's index among all DXGI adapters
int dxgi_index(uint64_t luid)
{
	IDXGIFactory1 * factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
		return -1;
	int found = -1;
	IDXGIAdapter1 * a = nullptr;
	for (UINT i = 0; found < 0 && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
	{
		DXGI_ADAPTER_DESC1 d{};
		if (SUCCEEDED(a->GetDesc1(&d)) && ((uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32 | d.AdapterLuid.LowPart) == luid))
			found = int(i);
		a->Release();
	}
	factory->Release();
	return found;
}

HMODULE nvcuda()
{
	static HMODULE lib = LoadLibraryW(L"nvcuda.dll"); // stays loaded (FFmpeg uses it too)
	return lib;
}

template <class F>
F cuda_symbol(const char * name)
{
	HMODULE lib = nvcuda();
	return lib ? reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(lib, name))) : nullptr;
}

// the CUDA ordinal of the GPU with this LUID
int cuda_device(uint64_t luid)
{
	auto init = cuda_symbol<int (*)(unsigned)>("cuInit");
	auto count_fn = cuda_symbol<int (*)(int *)>("cuDeviceGetCount");
	auto get = cuda_symbol<int (*)(int *, int)>("cuDeviceGet");
	auto get_luid = cuda_symbol<int (*)(char *, unsigned *, int)>("cuDeviceGetLuid");
	int count = 0;
	if (!init || !count_fn || !get || !get_luid || init(0) || count_fn(&count))
		throw std::runtime_error("NVIDIA CUDA driver unavailable (nvcuda.dll)");
	for (int i = 0; i < count; ++i)
	{
		int dev = 0;
		char id[8]{};
		unsigned mask = 0;
		uint64_t value = 0;
		if (!get(&dev, i) && !get_luid(id, &mask, dev))
		{
			std::memcpy(&value, id, 8);
			if (value == luid)
				return i;
		}
	}
	throw std::runtime_error("no CUDA device has the VR GPU's LUID");
}
} // namespace

// The few CUDA driver API calls for importing Vulkan memory (cuda.h types, stable ABI), from nvcuda.dll at runtime
struct hardware_encoder::cuda_state
{
	using CUresult = int;
	using CUdeviceptr = unsigned long long;
	using CUexternalMemory = void *;
	struct handle_desc
	{
		int type; // CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32 = 2
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

	cuda_state(uint64_t luid, int width, int height, AVPixelFormat sw_format)
	{
		import_memory = cuda_symbol<decltype(import_memory)>("cuImportExternalMemory");
		mapped_buffer = cuda_symbol<decltype(mapped_buffer)>("cuExternalMemoryGetMappedBuffer");
		destroy_memory = cuda_symbol<decltype(destroy_memory)>("cuDestroyExternalMemory");
		mem_free = cuda_symbol<decltype(mem_free)>("cuMemFree_v2");
		push = cuda_symbol<decltype(push)>("cuCtxPushCurrent_v2");
		pop = cuda_symbol<decltype(pop)>("cuCtxPopCurrent_v2");
		if (!import_memory || !mapped_buffer || !destroy_memory || !mem_free || !push || !pop)
			throw std::runtime_error("nvcuda.dll lacks external memory support");
		check(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, std::to_string(cuda_device(luid)).c_str(), nullptr, 0), "CUDA device");
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
		hd.type = 2;
		hd.handle.win32.handle = f.fd; // an NT handle stays ours: CUDA duplicates what it needs
		hd.size = f.size;
		hd.flags = 1;
		buffer b;
		if (CUresult r = import_memory(&b.memory, &hd); r != 0)
			throw std::runtime_error(std::format("cuImportExternalMemory failed ({})", r));
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

// Desktop frames that stay on the GPU: the capture's BGRA texture -> D3D11 video processor -> an NV12/P010 frame of a
// D3D11 frames pool on the same device, which NVENC / AMF / Media Foundation encode directly
struct hardware_encoder::d3d11_state
{
	ID3D11Device * device = nullptr;
	ID3D11DeviceContext * context = nullptr;
	ID3D11VideoDevice * video = nullptr;
	ID3D11VideoContext * video_context = nullptr;
	ID3D11VideoProcessorEnumerator * enumerator = nullptr;
	ID3D11VideoProcessor * processor = nullptr;
	AVBufferRef * hw_device = nullptr;
	AVBufferRef * frames = nullptr;
	std::map<void *, ID3D11VideoProcessorInputView *> inputs;
	std::map<std::pair<void *, intptr_t>, ID3D11VideoProcessorOutputView *> outputs;

	bool rgb = false; // BGRA frames straight to the encoder (NVENC converts RGB itself): a copy, no video processor

	d3d11_state(ID3D11Device * dev, int width, int height, AVPixelFormat sw_format, bool full_range) :
	        device(dev), rgb(sw_format == AV_PIX_FMT_BGRA)
	{
		device->AddRef();
		device->GetImmediateContext(&context);
		if (!rgb)
			setup_video_processor(width, height, sw_format, full_range);
		init_frames(width, height, sw_format);
	}

	void setup_video_processor(int width, int height, AVPixelFormat sw_format, bool full_range)
	{
		if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void **>(&video))) ||
		    FAILED(context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void **>(&video_context))))
			throw std::runtime_error("the capture device has no video processor");
		D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
		cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		cd.InputWidth = cd.OutputWidth = UINT(width);
		cd.InputHeight = cd.OutputHeight = UINT(height);
		cd.InputFrameRate = cd.OutputFrameRate = {60, 1};
		cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
		if (FAILED(video->CreateVideoProcessorEnumerator(&cd, &enumerator)) || FAILED(video->CreateVideoProcessor(enumerator, 0, &processor)))
			throw std::runtime_error("cannot create a D3D11 video processor");
		const DXGI_FORMAT out_format = sw_format == AV_PIX_FMT_P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
		UINT support = 0;
		if (FAILED(enumerator->CheckVideoProcessorFormat(out_format, &support)) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
			throw std::runtime_error(std::format("the video processor cannot output {}", av_get_pix_fmt_name(sw_format)));
		// RGB (full range) in, BT.709 YCbCr in the stream's range out
		ID3D11VideoContext1 * vc1 = nullptr;
		if (SUCCEEDED(video_context->QueryInterface(__uuidof(ID3D11VideoContext1), reinterpret_cast<void **>(&vc1))))
		{
			vc1->VideoProcessorSetStreamColorSpace1(processor, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
			vc1->VideoProcessorSetOutputColorSpace1(processor, full_range ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
			                                                              : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
			vc1->Release();
		}
		else
		{
			D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
			in.RGB_Range = 0; // full
			out.YCbCr_Matrix = 1; // BT.709
			out.Nominal_Range = full_range ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
			video_context->VideoProcessorSetStreamColorSpace(processor, 0, &in);
			video_context->VideoProcessorSetOutputColorSpace(processor, &out);
		}
		video_context->VideoProcessorSetStreamAutoProcessingMode(processor, 0, FALSE);
	}

	void init_frames(int width, int height, AVPixelFormat sw_format)
	{
		hw_device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
		auto * dc = static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(hw_device->data)->hwctx);
		dc->device = device;
		device->AddRef(); // FFmpeg releases its reference
		check(av_hwdevice_ctx_init(hw_device), "D3D11 device");
		frames = av_hwframe_ctx_alloc(hw_device);
		auto * fc = reinterpret_cast<AVHWFramesContext *>(frames->data);
		fc->format = AV_PIX_FMT_D3D11;
		fc->sw_format = sw_format;
		fc->width = width;
		fc->height = height;
		fc->initial_pool_size = 8;
		static_cast<AVD3D11VAFramesContext *>(fc->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET; // video processor output / copy target
		check(av_hwframe_ctx_init(frames), "D3D11 frames");
	}

	~d3d11_state()
	{
		for (auto & [k, v]: inputs)
			v->Release();
		for (auto & [k, v]: outputs)
			v->Release();
		av_buffer_unref(&frames);
		av_buffer_unref(&hw_device);
		if (processor)
			processor->Release();
		if (enumerator)
			enumerator->Release();
		if (video_context)
			video_context->Release();
		if (video)
			video->Release();
		if (context)
			context->Release();
		device->Release();
	}

	// texture (BGRA) -> a new frame of the pool
	AVFrame * convert(ID3D11Texture2D * texture)
	{
		AVFrame * out = av_frame_alloc();
		check(av_hwframe_get_buffer(frames, out, 0), "D3D11 frame");
		auto * target = reinterpret_cast<ID3D11Texture2D *>(out->data[0]);
		const intptr_t slice = reinterpret_cast<intptr_t>(out->data[1]);
		if (rgb) // the captured image into the pool texture's slice; the encoder converts it
		{
			context->CopySubresourceRegion(target, UINT(slice), 0, 0, 0, texture, 0, nullptr);
			return out;
		}
		ID3D11VideoProcessorInputView *& in = inputs[texture];
		if (!in)
		{
			D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
			d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
			check(SUCCEEDED(video->CreateVideoProcessorInputView(texture, enumerator, &d, &in)) ? 0 : AVERROR_EXTERNAL, "input view");
		}
		ID3D11VideoProcessorOutputView *& view = outputs[{target, slice}];
		if (!view)
		{
			D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
			d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
			d.Texture2DArray.FirstArraySlice = UINT(slice);
			d.Texture2DArray.ArraySize = 1;
			check(SUCCEEDED(video->CreateVideoProcessorOutputView(target, enumerator, &d, &view)) ? 0 : AVERROR_EXTERNAL, "output view");
		}
		D3D11_VIDEO_PROCESSOR_STREAM stream{};
		stream.Enable = TRUE;
		stream.pInputSurface = in;
		if (FAILED(video_context->VideoProcessorBlt(processor, view, 0, 1, &stream)))
		{
			av_frame_free(&out);
			throw std::runtime_error("VideoProcessorBlt failed");
		}
		return out;
	}
};

hardware_encoder::hardware_encoder(encoder_config c) :
        cfg(std::move(c))
{
	if (cfg.render_node.empty())
		cfg.render_node = default_render_node();
}

hardware_encoder::~hardware_encoder()
{
	sws_freeContext(sws);
	av_frame_free(&last);
	avcodec_free_context(&codec);
	cuda.reset();
	d3d.reset();
	copier.reset();
	av_buffer_unref(&copy_frames);
	av_buffer_unref(&copy_device);
	av_buffer_unref(&hw_device);
}

void hardware_encoder::note(const std::string & line)
{
	if (log)
		log(line);
}

std::string hardware_encoder::input_path() const
{
	if (!selected)
		return "not initialized";
	if (src_opaque)
		return std::format("{}, {}", selected, cuda_input ? "CUDA input" : copy_input ? "GPU copy into D3D11 frames" : "GPU readback");
	if (d3d_input)
		return d3d && d3d->rgb ? std::format("{}, GPU (Desktop Duplication -> RGB into the encoder)", selected)
		                       : std::format("{}, GPU (Desktop Duplication -> D3D11 video processor{})", selected, eight_bit_input ? ", NV12 -> 10-bit" : "");
	if (!codec) // set up with the first frame: GPU frames unless that fails (logged then)
		return std::format("{}, Desktop Duplication", selected);
	return std::format("{}, CPU (Desktop Duplication readback)", selected);
}

// opens the first working encoder for `input` (a software format, or CUDA frames of `frames`)
void hardware_encoder::open_codec(AVPixelFormat input, AVBufferRef * frames)
{
	const int w = cfg.width, h = cfg.height;
	const auto gpu = win::adapter(cfg.render_node);
	const unsigned vendor = gpu ? gpu->vendor : 0;
	if (!hw_device && gpu && !frames)
	{
		// a D3D11 device on the chosen adapter picks the GPU for NVENC / AMF / QSV (sw frames are uploaded there)
		const int index = dxgi_index(gpu->luid);
		if (index >= 0)
			av_hwdevice_ctx_create(&hw_device, AV_HWDEVICE_TYPE_D3D11VA, std::to_string(index).c_str(), nullptr, 0);
	}
	std::string errors;
	for (const auto & c: ordered_candidates(cfg.codec, vendor))
	{
		const bool d3d_frames = frames && reinterpret_cast<AVHWFramesContext *>(frames->data)->format == AV_PIX_FMT_D3D11;
		if (frames && !d3d_frames && std::strcmp(c.backend, "NVENC") != 0)
			continue; // CUDA frames: NVENC only
		if (d3d_frames && (!c.hardware || std::strcmp(c.backend, "QSV") == 0))
			continue; // D3D11 frames: NVENC, AMF, Media Foundation
		const AVCodec * def = avcodec_find_encoder_by_name(c.name);
		if (!def)
			continue;
		AVCodecContext * ctx = avcodec_alloc_context3(def);
		if (!ctx)
			continue;
		ctx->width = w;
		ctx->height = h;
		ctx->time_base = {1, std::max(cfg.fps, 1)};
		ctx->framerate = {std::max(cfg.fps, 1), 1};
		ctx->pix_fmt = frames ? reinterpret_cast<AVHWFramesContext *>(frames->data)->format : input;
		if (frames)
			ctx->hw_frames_ctx = av_buffer_ref(frames);
		else if (hw_device && c.hardware && std::strcmp(c.backend, "Media Foundation") != 0)
			ctx->hw_device_ctx = av_buffer_ref(hw_device);
		ctx->bit_rate = int64_t(std::max(cfg.kbps, 500)) * 1000;
		ctx->rc_max_rate = ctx->bit_rate;
		ctx->rc_buffer_size = int(ctx->bit_rate / std::max(cfg.fps, 1) * 2);
		ctx->gop_size = std::max(cfg.fps, 1) * 10; // IDR on demand (ForceKeyFrame) + safety net, like Linux
		ctx->max_b_frames = 0;
		ctx->thread_count = 1;
		ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
		ctx->color_range = cfg.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		ctx->colorspace = rgb_input ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709; // NVENC converts RGB input with BT.601
		ctx->color_primaries = AVCOL_PRI_BT709;
		ctx->color_trc = AVCOL_TRC_BT709;
		if (is_hevc(cfg.codec))
			ctx->profile = is_10bit(cfg.codec) ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_HEVC_MAIN;
		else if (is_av1(cfg.codec))
			ctx->profile = AV_PROFILE_AV1_MAIN; // Main covers 8- and 10-bit 4:2:0
		else
			ctx->profile = AV_PROFILE_H264_HIGH;
		tune(ctx, c.name, cfg);
		if (eight_bit_input)
			set_option(ctx, "highbitdepth", "1"); // NVENC: 10-bit encode of 8-bit input
		const int err = avcodec_open2(ctx, def, nullptr);
		if (err < 0)
		{
			errors += std::format(" {}: {};", c.name, av_error(err));
			avcodec_free_context(&ctx);
			continue;
		}
		codec = ctx;
		codec_def = def;
		selected = c.backend;
		note(std::format("[encode] {} {}x{} @{} fps, {} kbit/s, {} range{}{} on {}", c.name, w, h, cfg.fps, cfg.kbps,
		                 cfg.full_range ? "full" : "limited", cfg.pre_encode ? ", 2-pass" : "", cfg.vbaq ? ", AQ" : "",
		                 gpu ? gpu->name : std::string("the default GPU")));
		return;
	}
	throw std::runtime_error("no usable encoder:" + errors);
}

void hardware_encoder::setup(const captured_frame & f)
{
	const int w = cfg.width, h = cfg.height;
	if (w <= 0 || h <= 0)
		throw std::runtime_error("invalid frame size");
	if (f.d3d11_texture && !codec)
	{
		if (!d3d_failed)
			try
			{
				setup_d3d11(f);
				return;
			}
			catch (const std::exception & e)
			{
				note(std::string("[encoder] desktop frames on the GPU unavailable (") + e.what() + "): reading them back");
				avcodec_free_context(&codec);
				d3d.reset();
				d3d_input = false;
				d3d_failed = true;
			}
		captured_frame cpu = f; // the CPU path from here on
		int stride = 0;
		cpu.data = read_texture(f, stride);
		cpu.stride = uint32_t(stride);
		cpu.d3d11_texture = nullptr;
		cpu.av_sw_format = AV_PIX_FMT_BGRA;
		return setup(cpu);
	}
	if (!f.opaque_buffer && f.data == nullptr)
		throw std::runtime_error("no frame memory");
	const AVPixelFormat src = f.av_sw_format < 0 ? AV_PIX_FMT_BGRA : AVPixelFormat(f.av_sw_format);
	if (codec && src == src_format && w == src_width && h == src_height && int(f.stride) == src_stride && f.opaque_buffer == src_opaque)
		return;

	sws_freeContext(sws);
	sws = nullptr;
	av_frame_free(&last);
	codec_def = nullptr;
	selected = nullptr;
	avcodec_free_context(&codec);
	cuda.reset();
	cuda_input = false;
	src_format = src;
	src_width = w;
	src_height = h;
	src_stride = int(f.stride);
	src_opaque = f.opaque_buffer;
	pts = 0;

	const AVPixelFormat dst = is_10bit(cfg.codec) ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
	// VR frames already in the codec's layout go to NVENC through CUDA without a copy
	const auto gpu = win::adapter(cfg.render_node);
	if (f.opaque_buffer && !cuda_failed && gpu && gpu->vendor == 0x10de && src == (is_10bit(cfg.codec) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12))
		try
		{
			cuda = std::make_unique<cuda_state>(gpu->luid, w, h, src);
			open_codec(dst, cuda->frames);
			cuda_input = true;
			return;
		}
		catch (const std::exception & e)
		{
			note(std::string("[encoder] CUDA input unavailable (") + e.what() + "): reading the frames back");
			avcodec_free_context(&codec);
			cuda.reset();
			cuda_failed = true;
		}

	// AMF / Media Foundation: a GPU copy into the encoder's D3D11 frames (NV12/P010 as the runtime wrote them)
	if (f.opaque_buffer && !copy_failed && src == (is_10bit(cfg.codec) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12))
		try
		{
			setup_vr_d3d11(dst);
			copy_input = true;
			return;
		}
		catch (const std::exception & e)
		{
			note(std::string("[encoder] GPU copy into D3D11 frames unavailable (") + e.what() + "): reading the frames back");
			avcodec_free_context(&codec);
			copier.reset();
			av_buffer_unref(&copy_frames);
			av_buffer_unref(&copy_device);
			copy_failed = true;
		}

	// NV12/P010 in (VR readback): copied plane by plane when the format matches, else converted
	if (src != dst || !f.uv_offset)
	{
		sws = sws_getContext(w, h, src, w, h, dst, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws)
			throw std::runtime_error("cannot convert the frame to NV12/P010");
		const int * table = sws_getCoefficients(SWS_CS_ITU709);
		// RGB input is full range; NV12/P010 from the runtime already has the stream's range
		sws_setColorspaceDetails(sws, table, f.uv_offset ? cfg.full_range : 1, table, cfg.full_range, 0, 1 << 16, 1 << 16);
	}
	open_codec(dst, nullptr);

	last = av_frame_alloc();
	if (!last)
		throw std::runtime_error("cannot allocate the encoder frame");
	last->format = dst;
	last->width = w;
	last->height = h;
	if (av_frame_get_buffer(last, 32) < 0)
		throw std::runtime_error("cannot allocate the encoder frame buffer");
	last->color_range = cfg.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

void hardware_encoder::setup_vr_d3d11(AVPixelFormat sw_format)
{
	const auto gpu = win::adapter(cfg.render_node);
	if (!gpu)
		throw std::runtime_error("no adapter for " + cfg.render_node);
	const int index = dxgi_index(gpu->luid);
	check(av_hwdevice_ctx_create(&copy_device, AV_HWDEVICE_TYPE_D3D11VA, std::to_string(index).c_str(), nullptr, 0), "D3D11 device");
	constexpr int pool = 8;
	copy_frames = av_hwframe_ctx_alloc(copy_device);
	auto * fc = reinterpret_cast<AVHWFramesContext *>(copy_frames->data);
	fc->format = AV_PIX_FMT_D3D11;
	fc->sw_format = sw_format;
	fc->width = cfg.width;
	fc->height = cfg.height;
	fc->initial_pool_size = pool; // one texture array: shared with Vulkan as a whole
	auto * hc = static_cast<AVD3D11VAFramesContext *>(fc->hwctx);
	hc->BindFlags = D3D11_BIND_SHADER_RESOURCE;
	hc->MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	check(av_hwframe_ctx_init(copy_frames), "D3D11 frames");
	if (!hc->texture)
		throw std::runtime_error("the D3D11 frame pool is not one texture array");
	IDXGIResource1 * resource = nullptr;
	HANDLE shared = nullptr;
	const bool ok = SUCCEEDED(hc->texture->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&resource))) &&
	                SUCCEEDED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared));
	if (resource)
		resource->Release();
	if (!ok)
		throw std::runtime_error("cannot share the D3D11 frame pool");
	try
	{
		copier = std::make_unique<opaque_buffer_copier>(cfg.render_node, shared, uint32_t(cfg.width), uint32_t(cfg.height), uint32_t(pool),
		                                                is_10bit(cfg.codec));
	}
	catch (...)
	{
		CloseHandle(shared);
		throw;
	}
	CloseHandle(shared); // Vulkan imported it
	open_codec(sw_format, copy_frames);
}

void hardware_encoder::setup_d3d11(const captured_frame & f)
{
	auto * device = static_cast<ID3D11Device *>(f.d3d11_device);
	const auto gpu = win::adapter(cfg.render_node);
	const bool nvidia = gpu && gpu->vendor == 0x10de, ten_bit = is_10bit(cfg.codec);
	// GPU routes for the desktop's BGRA frames, best first:
	//   1. video processor -> NV12 / P010 (BT.709, the stream's range)
	//   2. NVIDIA, 10-bit codec: video processor -> NV12, NVENC encodes it as 10-bit ("highbitdepth"; the desktop is
	//      8-bit anyway) - for video processors without P010 output
	//   3. NVIDIA: BGRA straight into NVENC, which converts RGB itself - always with the BT.601 matrix, so the stream
	//      is labelled BT.601
	struct route
	{
		AVPixelFormat format;
		bool eight_bit_into_10;
		const char * name;
	};
	std::vector<route> routes{{ten_bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12, false, ten_bit ? "P010" : "NV12"}};
	if (nvidia && ten_bit)
		routes.push_back({AV_PIX_FMT_NV12, true, "NV12 into NVENC's 10-bit mode"});
	if (nvidia)
		routes.push_back({AV_PIX_FMT_BGRA, ten_bit, "RGB into NVENC"});
	std::string errors;
	for (size_t i = 0; i < routes.size(); ++i)
	{
		const route & r = routes[i];
		try
		{
			eight_bit_input = r.eight_bit_into_10;
			rgb_input = r.format == AV_PIX_FMT_BGRA;
			d3d = std::make_unique<d3d11_state>(device, cfg.width, cfg.height, r.format, cfg.full_range);
			open_codec(r.format, d3d->frames);
			if (i > 0)
				note(std::format("[encoder] desktop frames on the GPU as {} ({})", r.name, errors));
			d3d_input = true;
			src_opaque = false;
			src_format = AV_PIX_FMT_BGRA;
			src_width = cfg.width;
			src_height = cfg.height;
			src_stride = 0;
			pts = 0;
			return;
		}
		catch (const std::exception & e)
		{
			errors += std::format("{}{}: {}", errors.empty() ? "" : "; ", r.name, e.what());
			avcodec_free_context(&codec);
			d3d.reset();
			eight_bit_input = rgb_input = false;
		}
	}
	throw std::runtime_error(errors);
}

// the capture texture on the CPU (staging copy + map), for encoders that can't take D3D11 frames
const uint8_t * hardware_encoder::read_texture(const captured_frame & f, int & stride)
{
	static thread_local std::vector<uint8_t> pixels;
	auto * device = static_cast<ID3D11Device *>(f.d3d11_device);
	auto * texture = static_cast<ID3D11Texture2D *>(f.d3d11_texture);
	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.MiscFlags = 0;
	ID3D11Texture2D * staging = nullptr;
	ID3D11DeviceContext * context = nullptr;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
		throw std::runtime_error("cannot read the captured frame back");
	device->GetImmediateContext(&context);
	context->CopyResource(staging, texture);
	D3D11_MAPPED_SUBRESOURCE map{};
	const HRESULT hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &map);
	if (SUCCEEDED(hr))
	{
		pixels.resize(size_t(map.RowPitch) * desc.Height);
		std::memcpy(pixels.data(), map.pData, pixels.size());
		stride = int(map.RowPitch);
		context->Unmap(staging, 0);
	}
	context->Release();
	staging->Release();
	if (FAILED(hr))
		throw std::runtime_error("cannot map the captured frame");
	return pixels.data();
}

void hardware_encoder::convert(const captured_frame & frame)
{
	if (!codec)
		setup(frame);
	if (d3d_input)
	{
		if (!frame.d3d11_texture)
			throw std::runtime_error("the encoder expects GPU frames");
		AVFrame * out = d3d->convert(static_cast<ID3D11Texture2D *>(frame.d3d11_texture));
		out->color_range = cfg.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		av_frame_free(&last);
		last = out;
		return;
	}
	if (frame.d3d11_texture) // GPU frames, CPU encoder
	{
		captured_frame cpu = frame;
		int stride = 0;
		cpu.data = read_texture(frame, stride);
		cpu.stride = uint32_t(stride);
		cpu.d3d11_texture = nullptr;
		cpu.av_sw_format = AV_PIX_FMT_BGRA;
		return convert(cpu);
	}
	if (cuda_input)
	{
		if (!cuda->buffers.count(frame.buffer_id) && cuda->buffers.size() >= 12)
			cuda->clear(); // the runtime's buffer pool was replaced
		const auto & b = cuda->import(frame);
		AVFrame * in = av_frame_alloc();
		in->format = AV_PIX_FMT_CUDA;
		in->width = cfg.width;
		in->height = cfg.height;
		in->hw_frames_ctx = av_buffer_ref(cuda->frames);
		in->buf[0] = av_buffer_ref(b.ref);
		in->data[0] = reinterpret_cast<uint8_t *>(b.ptr);
		in->linesize[0] = int(frame.stride);
		in->data[1] = reinterpret_cast<uint8_t *>(b.ptr + frame.uv_offset); // chroma right after the luma rows
		in->linesize[1] = int(frame.stride);
		in->color_range = cfg.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		av_frame_free(&last);
		last = in;
		return;
	}
	if (copy_input)
	{
		AVFrame * out = av_frame_alloc();
		check(av_hwframe_get_buffer(copy_frames, out, 0), "D3D11 frame");
		copier->copy(frame.fd, frame.size, frame.buffer_bytes, frame.buffer_id, frame.stride, frame.uv_offset,
		             uint32_t(reinterpret_cast<intptr_t>(out->data[1])));
		out->color_range = cfg.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		av_frame_free(&last);
		last = out;
		return;
	}
	const uint8_t * pixels = frame.data;
	if (frame.opaque_buffer)
	{
		if (!opaque)
			opaque = std::make_unique<opaque_buffer_reader>(cfg.render_node);
		pixels = opaque->read(frame.fd, frame.size, frame.buffer_bytes, frame.buffer_id);
	}
	check(av_frame_make_writable(last), "encoder frame");
	if (sws)
	{
		const uint8_t * src[4] = {pixels, frame.uv_offset ? pixels + frame.uv_offset : nullptr, nullptr, nullptr};
		int strides[4] = {int(frame.stride), frame.uv_offset ? int(frame.stride) : 0, 0, 0};
		sws_scale(sws, src, strides, 0, cfg.height, last->data, last->linesize);
		return;
	}
	// same layout: luma rows, then interleaved chroma rows
	const int bytes_per_sample = is_10bit(cfg.codec) ? 2 : 1;
	av_image_copy_plane(last->data[0], last->linesize[0], pixels, int(frame.stride), cfg.width * bytes_per_sample, cfg.height);
	av_image_copy_plane(last->data[1], last->linesize[1], pixels + frame.uv_offset, int(frame.stride), cfg.width * bytes_per_sample,
	                    cfg.height / 2);
}

void hardware_encoder::prime(const captured_frame & f)
{
	if (!codec)
		setup(f);
	if (!cuda_input)
		return;
	// one trial encode through CUDA (the output is dropped; the stream starts with an IDR anyway)
	try
	{
		convert(f);
		send_frame(true, [](bytes &&, bool) {});
	}
	catch (const std::exception & e)
	{
		note(std::string("[encoder] NVENC from CUDA failed (") + e.what() + "): reading the frames back");
		av_frame_free(&last);
		avcodec_free_context(&codec);
		cuda.reset();
		cuda_input = false;
		cuda_failed = true;
		setup(f);
	}
}

void hardware_encoder::stage(const captured_frame & f)
{
	convert(f);
}

void hardware_encoder::send_frame(bool force_idr, const au_callback & cb)
{
	if (!codec || !last)
		throw std::runtime_error("encoder is not initialized");
	AVFrame * frame = last;
	AVFrame * clone = nullptr;
	if (cuda_input || d3d_input || copy_input) // hw frames: the encoder may hold on to one; send a reference, keep ours
		frame = clone = av_frame_clone(last);
	frame->pts = pts++;
	if (force_idr)
	{
		frame->pict_type = AV_PICTURE_TYPE_I;
		frame->flags |= AV_FRAME_FLAG_KEY;
	}
	else
	{
		frame->pict_type = AV_PICTURE_TYPE_NONE;
		frame->flags &= ~AV_FRAME_FLAG_KEY;
	}
	const int err = avcodec_send_frame(codec, frame);
	av_frame_free(&clone);
	if (err < 0)
		throw std::runtime_error(std::format("{} rejected a frame: {}", selected ? selected : "encoder", av_error(err)));
	while (true)
	{
		AVPacket * pkt = av_packet_alloc();
		if (!pkt)
			throw std::runtime_error("cannot allocate a packet");
		const int r = avcodec_receive_packet(codec, pkt);
		if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
		{
			av_packet_free(&pkt);
			break;
		}
		if (r < 0)
		{
			av_packet_free(&pkt);
			throw std::runtime_error(std::format("{} encode error: {}", selected ? selected : "encoder", av_error(r)));
		}
		bytes au;
		// AV1 "low overhead" stream for MediaCodec: each temporal unit starts with a temporal delimiter OBU
		if (is_av1(cfg.codec) && pkt->size > 0 && ((pkt->data[0] >> 3) & 0x0f) != 2)
			au = {0x12, 0x00};
		au.insert(au.end(), pkt->data, pkt->data + pkt->size);
		const bool sync = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
		av_packet_free(&pkt);
		if (!au.empty())
			cb(std::move(au), sync);
	}
}

void hardware_encoder::encode(const captured_frame & f, bool force_idr, const au_callback & cb)
{
	convert(f);
	send_frame(force_idr, cb);
}

bool hardware_encoder::reencode_last(bool force_idr, const au_callback & cb)
{
	if (!codec || !last)
		return false;
	send_frame(force_idr, cb);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Free functions. Windows has no DRM render nodes: "luid:<hex>" names a DXGI adapter (win_gpu.h).

std::string default_render_node()
{
	if (const char * node = std::getenv("OPENDESK_RENDER_NODE"); node && *node)
		return node;
	auto a = win::adapter({}); // the first adapter drives the primary display
	return a ? win::node_for(a->luid) : std::string();
}

std::string vr_render_node()
{
	return win::vr_node();
}

// (dual-GPU use is Linux only for now)
std::string vr_encode_node() { return vr_render_node(); }
std::string desktop_encode_node() { return default_render_node(); }
void set_gpu_selection(const std::string &, const std::string &) {}
std::vector<gpu_info> list_gpus() { return {}; }

unsigned gpu_vendor(const std::string & node)
{
	auto a = win::adapter(node);
	return a ? a->vendor : 0;
}

std::string hardware_encoder_probe(const std::string & node)
{
	try
	{
		hardware_encoder enc({256, 256, 60, 1000, video_codec::h264, node});
		std::vector<uint8_t> pixels(256 * 256 * 4, 0);
		captured_frame f;
		f.data = pixels.data();
		f.stride = 256 * 4;
		f.av_sw_format = AV_PIX_FMT_BGRA;
		bool got = false;
		for (int i = 0; i < 8 && !got; ++i)
			enc.encode(f, i == 0, [&](bytes &&, bool) { got = true; });
		return got ? "" : "the encoder produced no frame";
	}
	catch (const std::exception & e)
	{
		return e.what();
	}
}

std::vector<video_codec> hardware_encoder_codecs(const std::string & node)
{
	std::vector<video_codec> out;
	for (video_codec c: {video_codec::h264, video_codec::hevc, video_codec::hevc_10bit, video_codec::av1, video_codec::av1_10bit})
	{
		try
		{
			// A real stream size: some encoders reject tiny frames for HEVC/AV1.
			constexpr int w = 1280, h = 720;
			hardware_encoder enc({w, h, 60, 5000, c, node});
			std::vector<uint8_t> pixels(size_t(w) * h * 4, 0);
			captured_frame f;
			f.data = pixels.data();
			f.stride = w * 4;
			f.av_sw_format = AV_PIX_FMT_BGRA;
			bool got = false;
			for (int i = 0; i < 8 && !got; ++i) // some encoders hand out their first packet a few frames later
				enc.encode(f, i == 0, [&](bytes &&, bool) { got = true; });
			if (got)
				out.push_back(c);
			else if (std::getenv("OPENDESK_DEBUG_CODEC_PROBE"))
				std::fprintf(stderr, "codec probe %s: no packet\n", codec_name(c));
		}
		catch (const std::exception & e)
		{
			if (std::getenv("OPENDESK_DEBUG_CODEC_PROBE"))
				std::fprintf(stderr, "codec probe %s: %s\n", codec_name(c), e.what());
		}
	}
	return out;
}
} // namespace od
