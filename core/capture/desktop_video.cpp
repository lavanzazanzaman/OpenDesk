#include "desktop_video.h"
#include "hardware_encoder.h"

#ifdef _WIN32
#include "win_capture.h"
#else
#include "pipewire_capture.h"
#include "x11_capture.h"

#include <unistd.h>
#endif

#include <chrono>
#include <format>
#include <utility>

namespace od
{
namespace
{
constexpr uint8_t source_desktop = 0;

int64_t now_us()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// not every GPU encodes every codec: HEVC 10-bit -> HEVC -> H.264, AV1 10-bit -> AV1 -> HEVC -> H.264
std::vector<video_codec> fallback_chain(video_codec wanted)
{
	std::vector<video_codec> chain{wanted};
	if (wanted == video_codec::av1_10bit)
		chain.push_back(video_codec::av1);
	if (is_av1(wanted) || wanted == video_codec::hevc_10bit)
		chain.push_back(video_codec::hevc);
	if (wanted != video_codec::h264)
		chain.push_back(video_codec::h264);
	return chain;
}
} // namespace

struct desktop_video_sender::stream_state
{
	int index = 0;
	desktop_stream_setup setup;
	int kbps = 0;
	std::mutex mutex; // PipeWire thread encodes; keyframe requests come from the engine thread
	std::unique_ptr<hardware_encoder> encoder;
	video_codec codec = video_codec::h264;
	int width = 0, height = 0;
	std::atomic<bool> idr{true};
	uint64_t frames = 0;
	std::chrono::steady_clock::time_point encode_start; // this pass
#ifdef _WIN32
	std::unique_ptr<win_capture> win; // last: stops before the encoder goes away
#else
	std::unique_ptr<pipewire_capture> capture; // last: stops before the encoder goes away
	std::unique_ptr<x11_capture> x11;
#endif
};

desktop_video_sender::desktop_video_sender(stream_channel & video_channel, desktop_video_params p,
                                           std::vector<desktop_stream_setup> setups, callbacks c) :
        channel(video_channel), params(std::move(p)), cb(std::move(c))
{
	if (setups.empty() || setups.size() > 4)
		throw std::runtime_error("1 to 4 screens can be streamed");
	// the headset's bitrate budget is shared by all screens, by area
	double total_area = 0;
	for (const auto & s: setups)
		total_area += double(std::max(s.expected_width, 1)) * std::max(s.expected_height, 1);
	for (size_t i = 0; i < setups.size(); ++i)
	{
		auto st = std::make_unique<stream_state>();
		st->index = int(i);
		st->setup = setups[i];
		double share = double(std::max(setups[i].expected_width, 1)) * std::max(setups[i].expected_height, 1) / total_area;
		st->kbps = std::max(1000, int(params.kbps * share));
		st->width = setups[i].expected_width;
		st->height = setups[i].expected_height;
		announced.push_back({0, 0, setups[i].bounds}); // nothing announced yet
		streams.push_back(std::move(st));
	}
	encoder_thread = std::thread(&desktop_video_sender::encode_loop, this);
	for (auto & st: streams)
	{
		stream_state * raw = st.get();
#ifdef _WIN32
		if (raw->setup.windows)
		{
			raw->win = std::make_unique<win_capture>(
			        raw->setup.name, params.fps,
			        [this, raw](const captured_frame & f, int w, int h) { on_frame(*raw, f, w, h); }, cb.log);
			cb.log(std::format("[capture] screen {} = {} (DXGI Desktop Duplication, monitor {}), bounds {},{} {}x{}", raw->index,
			                   raw->setup.name, raw->setup.monitor_index, raw->setup.bounds.x, raw->setup.bounds.y,
			                   raw->setup.bounds.width, raw->setup.bounds.height));
			continue;
		}
#else
		if (raw->setup.x11)
		{
			raw->x11 = std::make_unique<x11_capture>(
			        raw->setup.x11_x, raw->setup.x11_y, raw->setup.expected_width, raw->setup.expected_height, params.fps,
			        [this, raw](const captured_frame & f, int w, int h) { on_frame(*raw, f, w, h); }, cb.log);
			cb.log(std::format("[capture] screen {} = {} (X11), bounds {},{} {}x{}", raw->index, raw->setup.name, raw->setup.bounds.x,
			                   raw->setup.bounds.y, raw->setup.bounds.width, raw->setup.bounds.height));
			continue;
		}
		int pw_fd = std::exchange(raw->setup.pipewire_fd, -1);
		raw->capture = std::make_unique<pipewire_capture>(
		        pw_fd, raw->setup.node_id, raw->setup.serial, params.fps,
		        [this, raw](const captured_frame & f, const pipewire_capture::format & fmt) { on_frame(*raw, f, fmt.width, fmt.height); },
		        cb.log);
		cb.log(std::format("[capture] screen {} = {} (node {}), bounds {},{} {}x{}", raw->index, raw->setup.name,
		                   raw->setup.node_id, raw->setup.bounds.x, raw->setup.bounds.y, raw->setup.bounds.width,
		                   raw->setup.bounds.height));
#endif
	}
}

desktop_video_sender::~desktop_video_sender()
{
	alive = false;
	for (auto & st: streams)
	{
#ifdef _WIN32
		st->win.reset(); // stop every capture thread before encoders and the socket go away
#else
		st->capture.reset(); // stop every PipeWire / X11 thread before encoders and the socket go away
		st->x11.reset();
#endif
	}
	wake();
	if (encoder_thread.joinable())
		encoder_thread.join();
	cb.log(std::format("[video] stream stopped after {} frames", frame_index));
}

bool desktop_video_sender::running() const
{
	if (!alive)
		return false;
	for (const auto & st: streams)
#ifdef _WIN32
		if (st->win && st->win->failed())
			return false;
#else
		if ((st->capture && st->capture->failed()) || (st->x11 && st->x11->failed()))
			return false;
#endif
	return true;
}

void desktop_video_sender::wake()
{
	{
		std::lock_guard lock(wake_mutex);
		wake_pending = true;
	}
	wake_cv.notify_one();
}

void desktop_video_sender::request_keyframe()
{
	// static screens: no new frame may come for a while, so re-send the last ones as IDRs right away
	for (auto & st: streams)
		st->idr = true;
	wake();
}

void desktop_video_sender::set_vibration(float strong, float weak)
{
	if (strong == vibration_left && weak == vibration_right)
		return;
	vibration_left = strong;
	vibration_right = weak;
	wake(); // the headset applies vibration per decoded main-screen frame: push one now even if nothing moves
}

void desktop_video_sender::encode_loop()
{
	using clock = std::chrono::steady_clock;
	// frames of different screens arrive unsynchronised: allow passes up to twice the frame rate, not one per
	// capture callback
	const auto min_interval = std::chrono::microseconds(500000 / std::max(params.fps, 1));
	clock::time_point last_pass{};
	while (true)
	{
		{
			std::unique_lock lock(wake_mutex);
			wake_cv.wait(lock, [this] { return wake_pending || !alive; });
			if (!alive)
				return;
		}
		if (auto next = last_pass + min_interval; clock::now() < next)
			std::this_thread::sleep_until(next);
		{
			std::lock_guard lock(wake_mutex);
			wake_pending = false;
		}
		last_pass = clock::now();
		if (!format_sent)
			continue;
		video_codec codec;
		{
			std::lock_guard fl(format_mutex);
			codec = *active_codec;
		}
		for (auto & st: streams) // main screen first: the headset presents once it has all of them
		{
			std::lock_guard lock(st->mutex);
			if (!st->encoder || st->codec != codec)
				continue;
			try
			{
				st->encode_start = clock::now();
				st->encoder->reencode_last(st->idr.exchange(false), [this, &s = *st](bytes && au, bool sync) { send_au(s, std::move(au), sync); });
			}
			catch (const std::exception & e)
			{
				cb.log(std::format("[video] screen {} encoder error: {}", st->index, e.what()));
				alive = false;
				return;
			}
		}
	}
}

void desktop_video_sender::open_encoder(stream_state & st, const captured_frame & f, int w, int h)
{
	st.encoder.reset();
	std::vector<video_codec> chain;
	{
		std::lock_guard lock(format_mutex);
		chain = active_codec ? std::vector{*active_codec} : fallback_chain(params.codec);
	}
	std::string errors;
	for (video_codec c: chain)
	{
		try
		{
			encoder_config ec{w, h, params.fps, st.kbps, c, default_render_node(), params.full_range};
			ec.pre_encode = params.pre_encode;
			ec.vbaq = params.vbaq;
			st.encoder = std::make_unique<hardware_encoder>(ec);
			st.encoder->prime(f); // open the codec now so an unsupported one fails here, not mid-stream
			st.codec = c;
			break;
		}
		catch (const std::exception & e)
		{
			st.encoder.reset();
			errors += std::format(" {}: {};", codec_name(c), e.what());
		}
	}
	if (!st.encoder)
		throw std::runtime_error("no usable hardware encoder:" + errors);
	if (st.codec != params.codec && st.index == 0)
		cb.log(std::format("[video] {} unavailable, using {} ({})", codec_name(params.codec), codec_name(st.codec), errors));
	st.width = w;
	st.height = h;
	st.idr = true;
}

void desktop_video_sender::on_frame(stream_state & st, const captured_frame & f, int w, int h)
{
	if (!alive)
		return;
	bool announce = false;
	{
		std::unique_lock lock(st.mutex);
		try
		{
			bool codec_mismatch = false;
			{
				std::lock_guard fl(format_mutex);
				codec_mismatch = st.encoder && active_codec && st.codec != *active_codec;
			}
			if (!st.encoder || w != st.width || h != st.height || codec_mismatch)
			{
				open_encoder(st, f, w, h);
				std::lock_guard fl(format_mutex);
				if (st.index == 0 && !active_codec) // the main screen picks the codec; the others follow
				{
					active_codec = st.codec;
					if (cb.active_codec)
						cb.active_codec(st.codec);
				}
				stream_format now{w, h, st.setup.bounds};
				if (announced[st.index] != now)
				{
					announced[st.index] = now;
					announce = true;
				}
			}
			if (announce)
			{
				std::lock_guard fl(format_mutex);
				announce = false;
				// announce once the main screen is known; extra screens not seen yet use their expected size
				if (active_codec && announced[0].width > 0)
				{
					std::vector<stream_format> all;
					for (const auto & s: streams)
					{
						stream_format a = announced[s->index];
						if (a.width == 0)
							a = {s->width, s->height, s->setup.bounds};
						all.push_back(a);
					}
					if (all == last_sent)
						goto encode; // another screen announced the same layout already
					last_sent = all;
					std::string sizes;
					for (const auto & a: all)
						sizes += std::format("{}{}x{}", sizes.empty() ? "" : " + ", a.width, a.height);
					{
						std::lock_guard sl(socket_mutex);
						socket_ok = socket_ok && channel.send(pack_video_format(source_desktop, *active_codec, params.fps, all));
					}
					cb.log(std::format("[video] -> VideoFormat Desktop {} {} @{}, {} kbps, {} range{}{} ({}, {})",
					                   codec_name(*active_codec), sizes, params.fps, params.kbps,
					                   params.full_range ? "full" : "limited", params.vbaq ? ", VBAQ" : "",
					                   params.pre_encode ? ", pre-encode" : "",
#ifdef _WIN32
					                   st.encoder->backend(), st.encoder->input_path()));
#else
					                   gpu_vendor(st.encoder->config().render_node) == 0x10de ? "NVENC" : "VAAPI", st.encoder->config().render_node));
#endif
					format_sent = true;
					announce = true;
				}
			}
		encode:
			if (announce) // every screen restarts with an IDR under the new format
				for (auto & other: streams)
					other->idr = true;
			st.encoder->stage(f);
		}
		catch (const std::exception & e)
		{
			cb.log(std::format("[video] screen {} encoder error: {}", st.index, e.what()));
			alive = false;
			return;
		}
	}
	wake();
}

void desktop_video_sender::send_au(stream_state & st, bytes && au, bool sync)
{
	if (!format_sent) // encoded only to have a last frame to refresh from
		return;
	std::lock_guard lock(socket_mutex);
	if (!socket_ok)
		return;
	bytes packet = st.index == 0 ? pack_frame_header(now_us(), frame_index, source_desktop, sync, st.width, st.height,
	                                                 int32_t(au.size()), 1000.f / float(params.fps), vibration_left,
	                                                 vibration_right)
	                             : pack_slice_header(now_us(), st.index, int32_t(au.size()));
	if (st.index == 0)
	{
		float encode_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - st.encode_start).count();
		set_frame_timing(packet, 1000.f / float(params.fps), 1000.f / float(params.fps), encode_ms);
	}
	size_t au_size = au.size();
	packet.insert(packet.end(), au.begin(), au.end());
	socket_ok = channel.send(packet);
	if (!socket_ok)
	{
		alive = false;
		return;
	}
	if (st.frames++ == 0)
		cb.log(std::format("[video] -> screen {} first frame {} {} bytes", st.index, sync ? "sync" : "NON-SYNC", au_size));
	if (st.index == 0)
		++frame_index;
	++window_frames;
	window_bytes += au_size;
	int64_t now = now_us();
	if (!window_start_us)
		window_start_us = now;
	if (double span = double(now - window_start_us) / 1e6; span >= 5)
	{
		cb.log(std::format("[video] {} frames, {:.1f} fps (all screens), {:.1f} Mbit/s", frame_index, window_frames / span,
		                   window_bytes * 8 / span / 1e6));
		window_frames = window_bytes = 0;
		window_start_us = now;
	}
}
} // namespace od
