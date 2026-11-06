#include "pipewire_capture.h"

#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <format>
#include <stdexcept>

namespace od
{
namespace
{
constexpr uint64_t drm_mod_linear = 0;

// SPA little-endian formats <-> DRM fourcc <-> FFmpeg memory layout
struct format_map
{
	spa_video_format spa;
	uint32_t drm;
	AVPixelFormat av;
};
constexpr format_map formats[] = {
        {SPA_VIDEO_FORMAT_BGRx, 0x34325258 /* XR24 */, AV_PIX_FMT_BGR0},
        {SPA_VIDEO_FORMAT_BGRA, 0x34325241 /* AR24 */, AV_PIX_FMT_BGRA},
        {SPA_VIDEO_FORMAT_RGBx, 0x34324258 /* XB24 */, AV_PIX_FMT_RGB0},
        {SPA_VIDEO_FORMAT_RGBA, 0x34324241 /* AB24 */, AV_PIX_FMT_RGBA},
};

const spa_pod * build_format(spa_pod_builder * b, spa_video_format f, bool dmabuf, int max_fps)
{
	spa_rectangle def{1920, 1080}, min{1, 1}, max{16384, 16384};
	spa_fraction fr_def{0, 1}, fr_min{0, 1}, fr_max{uint32_t(max_fps), 1};
	spa_pod_frame obj;
	spa_pod_builder_push_object(b, &obj, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
	                    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(f), 0);
	if (dmabuf)
	{
		// one modifier (linear): nothing left to fixate, and every VAAPI driver can import it
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_long(b, int64_t(drm_mod_linear));
	}
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def, &min, &max),
	                    SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fr_def),
	                    SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&fr_max, &fr_min, &fr_max), 0);
	return static_cast<const spa_pod *>(spa_pod_builder_pop(b, &obj));
}
} // namespace

pipewire_capture::pipewire_capture(int pipewire_fd, uint32_t node_id, uint64_t serial, int max_fps, frame_callback f,
                                   log_callback l) :
        on_frame(std::move(f)), log(std::move(l))
{
	pw_init(nullptr, nullptr);
	loop = pw_thread_loop_new("vd-capture", nullptr);
	if (!loop) // e.g. a libpipewire that can't find its SPA plugins
		throw std::runtime_error("cannot create a PipeWire loop (PipeWire client library/plugins missing?)");
	context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
	if (!context || pw_thread_loop_start(loop) < 0)
	{
		if (context)
			pw_context_destroy(context);
		pw_thread_loop_destroy(loop);
		throw std::runtime_error("cannot start PipeWire loop");
	}

	pw_thread_loop_lock(loop);
	core = pipewire_fd >= 0 ? pw_context_connect_fd(context, pipewire_fd, nullptr, 0) // takes the fd
	                        : pw_context_connect(context, nullptr, 0);
	if (!core)
	{
		pw_thread_loop_unlock(loop);
		throw std::runtime_error("cannot connect to PipeWire");
	}
	pw_properties * props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
	                                          PW_KEY_MEDIA_ROLE, "Screen", nullptr);
	uint32_t target = node_id;
	if (serial)
	{
		pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%llu", (unsigned long long)serial);
		target = PW_ID_ANY;
	}
	stream = pw_stream_new(core, "OpenDesk capture", props);
	static const pw_stream_events events = [] {
		pw_stream_events e{};
		e.version = PW_VERSION_STREAM_EVENTS;
		e.state_changed = [](void * d, pw_stream_state o, pw_stream_state s, const char * err) {
			on_state_changed(d, int(o), int(s), err);
		};
		e.param_changed = on_param_changed;
		e.process = on_process;
		return e;
	}();
	listener = new spa_hook{};
	pw_stream_add_listener(stream, listener, &events, this);

	uint8_t buffer[4096];
	spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	std::vector<const spa_pod *> params;
	for (bool dmabuf: {true, false}) // DMA-BUF first: zero copy
		for (const auto & m: formats)
			params.push_back(build_format(&b, m.spa, dmabuf, max_fps));
	int err = pw_stream_connect(stream, PW_DIRECTION_INPUT, target,
	                            pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
	                            params.data(), uint32_t(params.size()));
	pw_thread_loop_unlock(loop);
	if (err < 0)
		throw std::runtime_error("cannot connect to the screen-cast stream");
}

pipewire_capture::~pipewire_capture()
{
	if (loop)
		pw_thread_loop_stop(loop);
	if (stream)
		pw_stream_destroy(stream);
	if (core)
		pw_core_disconnect(core);
	if (context)
		pw_context_destroy(context);
	if (loop)
		pw_thread_loop_destroy(loop);
	delete listener;
}

void pipewire_capture::on_state_changed(void * data, int, int state, const char * err)
{
	auto * self = static_cast<pipewire_capture *>(data);
	if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
	{
		self->error = true;
		self->log(std::format("[capture] PipeWire stream {}{}", state == PW_STREAM_STATE_ERROR ? "error: " : "disconnected",
		                      err ? err : ""));
	}
}

void pipewire_capture::on_param_changed(void * data, uint32_t id, const spa_pod * param)
{
	auto * self = static_cast<pipewire_capture *>(data);
	if (!param || id != SPA_PARAM_Format)
		return;
	spa_video_info_raw raw{};
	if (spa_format_video_raw_parse(param, &raw) < 0)
		return;
	format f;
	f.width = int(raw.size.width);
	f.height = int(raw.size.height);
	f.dmabuf = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;
	f.modifier = raw.modifier;
	for (const auto & m: formats)
		if (m.spa == raw.format)
		{
			f.drm_format = m.drm;
			f.av_sw_format = m.av;
		}
	self->fmt = f;
	self->log(std::format("[capture] {}x{} {} via {}", f.width, f.height,
	                      spa_debug_type_find_short_name(spa_type_video_format, raw.format), f.dmabuf ? "DMA-BUF" : "shared memory"));

	// tell the producer which buffer types we accept for the negotiated format
	uint8_t buffer[1024];
	spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	int types = f.dmabuf ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
	const spa_pod * params[] = {static_cast<const spa_pod *>(spa_pod_builder_add_object(
	        &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
	        SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(types)))};
	pw_stream_update_params(self->stream, params, 1);
}

void pipewire_capture::on_process(void * data)
{
	auto * self = static_cast<pipewire_capture *>(data);
	pw_buffer * newest = nullptr;
	// drain to the newest buffer: never encode a stale frame if we fell behind
	while (pw_buffer * b = pw_stream_dequeue_buffer(self->stream))
	{
		if (newest)
			pw_stream_queue_buffer(self->stream, newest);
		newest = b;
	}
	if (!newest)
		return;
	spa_buffer * buf = newest->buffer;
	spa_data & d = buf->datas[0];
	if (d.chunk->size > 0 && !(d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) && self->fmt.width > 0)
	{
		captured_frame f;
		f.offset = d.chunk->offset;
		f.stride = uint32_t(d.chunk->stride);
		f.size = d.maxsize;
		f.drm_format = self->fmt.drm_format;
		f.av_sw_format = self->fmt.av_sw_format;
		f.modifier = self->fmt.modifier;
		if (d.type == SPA_DATA_DmaBuf)
			f.fd = int(d.fd);
		else if (d.data)
			f.data = static_cast<const uint8_t *>(d.data) + d.chunk->offset;
		if (f.fd >= 0 || f.data)
			self->on_frame(f, self->fmt);
	}
	pw_stream_queue_buffer(self->stream, newest);
}
} // namespace od
