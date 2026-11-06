// Windows encoder: captured BGRA frames (DXGI Desktop Duplication) -> NV12/P010 -> the first working hardware
// encoder among NVENC, AMF and QSV, then Media Foundation, then libx264/libx265. One access unit per packet, no
// B-frames and no lookahead so the headset can present every frame as soon as it arrives.
#include "hardware_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
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

void set_option(AVCodecContext * ctx, const char * name, const char * value)
{
	av_opt_set(ctx->priv_data, name, value, 0); // options a backend does not know are ignored
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
		set_option(ctx, "forced-idr", "1");
		set_option(ctx, "surfaces", "4");
		// "2-Pass encoding": NVENC's pre-analysis pass at quarter resolution gives most of the rate-control
		// gain for a fraction of the GPU time (same mapping as the Linux NVENC path).
		set_option(ctx, "multipass", cfg.pre_encode ? "qres" : "disabled");
		set_option(ctx, "spatial-aq", cfg.vbaq ? "1" : "0");
	}
	else if (std::strstr(encoder, "amf"))
	{
		set_option(ctx, "usage", "ultralowlatency");
		set_option(ctx, "quality", "speed");
		set_option(ctx, "rc", "cbr");
		set_option(ctx, "enforce_hrd", "1");
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
} // namespace

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
	return std::format("{}, CPU (Desktop Duplication)", selected);
}

// Frames arrive as BGRA at the desktop's stride. P010 for 10-bit HEVC/AV1, NV12 otherwise.
void hardware_encoder::setup(const captured_frame & f)
{
	const int w = cfg.width, h = cfg.height;
	if (w <= 0 || h <= 0)
		throw std::runtime_error("invalid frame size");
	if (f.data == nullptr)
		throw std::runtime_error("no frame memory");
	const AVPixelFormat src = f.av_sw_format < 0 ? AV_PIX_FMT_BGRA : AVPixelFormat(f.av_sw_format);
	if (codec && src == src_format && w == src_width && h == src_height && int(f.stride) == src_stride)
		return;

	sws_freeContext(sws);
	sws = nullptr;
	av_frame_free(&last);
	codec_def = nullptr;
	selected = nullptr;
	avcodec_free_context(&codec);
	src_format = src;
	src_width = w;
	src_height = h;
	src_stride = int(f.stride);
	pts = 0;

	const AVPixelFormat dst = is_10bit(cfg.codec) ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
	sws = sws_getContext(w, h, src, w, h, dst, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws)
		throw std::runtime_error("cannot convert the captured frame to NV12/P010");
	if (cfg.full_range)
	{
		const int * table = sws_getCoefficients(SWS_CS_DEFAULT);
		sws_setColorspaceDetails(sws, table, 1, table, 1, 0, 1 << 16, 1 << 16);
	}

	std::string errors;
	for (const auto & c: candidates(cfg.codec))
	{
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
		ctx->pix_fmt = dst;
		ctx->bit_rate = int64_t(std::max(cfg.kbps, 500)) * 1000;
		ctx->rc_max_rate = ctx->bit_rate;
		ctx->rc_buffer_size = int(ctx->bit_rate / std::max(cfg.fps, 1) * 2);
		ctx->gop_size = std::max(cfg.fps, 1) * 2;
		ctx->max_b_frames = 0;
		ctx->thread_count = 1;
		ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
		ctx->color_range = cfg.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		if (is_hevc(cfg.codec))
			ctx->profile = is_10bit(cfg.codec) ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_HEVC_MAIN;
		else if (is_av1(cfg.codec))
			ctx->profile = is_10bit(cfg.codec) ? AV_PROFILE_AV1_HIGH : AV_PROFILE_AV1_MAIN;
		else
			ctx->profile = AV_PROFILE_H264_HIGH;
		tune(ctx, c.name, cfg);
		const int err = avcodec_open2(ctx, def, nullptr);
		if (err < 0)
		{
			char buf[AV_ERROR_MAX_STRING_SIZE]{};
			av_strerror(err, buf, sizeof(buf));
			errors += std::format(" {}: {};", c.name, buf);
			avcodec_free_context(&ctx);
			continue;
		}
		codec = ctx;
		codec_def = def;
		selected = c.backend;
		note(std::format("[encode] {} {}x{} @{} fps, {} kbit/s, {} range{}{}", c.name, w, h, cfg.fps, cfg.kbps,
		                 cfg.full_range ? "full" : "limited", cfg.pre_encode ? ", 2-pass" : "", cfg.vbaq ? ", AQ" : ""));
		break;
	}
	if (!codec)
		throw std::runtime_error("no usable encoder:" + errors);

	last = av_frame_alloc();
	if (!last)
		throw std::runtime_error("cannot allocate the encoder frame");
	last->format = dst;
	last->width = w;
	last->height = h;
	last->color_range = cfg.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
	if (av_frame_get_buffer(last, 32) < 0)
		throw std::runtime_error("cannot allocate the encoder frame buffer");
	last->color_range = cfg.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

void hardware_encoder::convert(const captured_frame & f)
{
	if (!codec)
		setup(f);
	const uint8_t * src[4] = {f.data, nullptr, nullptr, nullptr};
	int strides[4] = {int(f.stride), 0, 0, 0};
	sws_scale(sws, src, strides, 0, cfg.height, last->data, last->linesize);
}

void hardware_encoder::prime(const captured_frame & f)
{
	if (!codec)
		setup(f);
}

void hardware_encoder::stage(const captured_frame & f)
{
	convert(f);
}

void hardware_encoder::send_frame(bool force_idr, const au_callback & cb)
{
	if (!codec || !last)
		throw std::runtime_error("encoder is not initialized");
	last->pts = pts++;
	if (force_idr)
	{
		last->pict_type = AV_PICTURE_TYPE_I;
		last->flags |= AV_FRAME_FLAG_KEY;
	}
	else
	{
		last->pict_type = AV_PICTURE_TYPE_NONE;
		last->flags &= ~AV_FRAME_FLAG_KEY;
	}
	const int err = avcodec_send_frame(codec, last);
	if (err < 0)
	{
		char buf[AV_ERROR_MAX_STRING_SIZE]{};
		av_strerror(err, buf, sizeof(buf));
		throw std::runtime_error(std::format("{} rejected a frame: {}", selected ? selected : "encoder", buf));
	}
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
			char buf[AV_ERROR_MAX_STRING_SIZE]{};
			av_strerror(r, buf, sizeof(buf));
			throw std::runtime_error(std::format("{} encode error: {}", selected ? selected : "encoder", buf));
		}
		bytes au(reinterpret_cast<const uint8_t *>(pkt->data), reinterpret_cast<const uint8_t *>(pkt->data) + pkt->size);
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
// Free functions. Windows has no DRM render nodes: the encoder backends are probed directly.

std::string default_render_node()
{
	return {};
}

std::string vr_render_node()
{
	return {};
}

unsigned gpu_vendor(const std::string &)
{
	return 0;
}

std::string hardware_encoder_probe(const std::string &)
{
	try
	{
		hardware_encoder enc({256, 256, 60, 1000, video_codec::h264, {}});
		std::vector<uint8_t> pixels(256 * 256 * 4, 0);
		captured_frame f;
		f.data = pixels.data();
		f.stride = 256 * 4;
		f.av_sw_format = AV_PIX_FMT_BGRA;
		bool got = false;
		enc.encode(f, true, [&](bytes &&, bool) { got = true; });
		return got ? "" : "the encoder produced no frame";
	}
	catch (const std::exception & e)
	{
		return e.what();
	}
}

std::vector<video_codec> hardware_encoder_codecs(const std::string &)
{
	std::vector<video_codec> out;
	for (video_codec c: {video_codec::h264, video_codec::hevc, video_codec::hevc_10bit, video_codec::av1, video_codec::av1_10bit})
	{
		try
		{
			// A real stream size: some encoders reject tiny frames for HEVC/AV1.
			constexpr int w = 1280, h = 720;
			hardware_encoder enc({w, h, 60, 5000, c, {}});
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
