#include "vr_sender.h"
#include "test_pattern.h"
#include "hardware_encoder.h"
#include "vr_ipc.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstdlib>
#include <format>
#include <future>

namespace od
{
namespace
{
captured_frame frame_of(const dmabuf_image & i)
{
	captured_frame f;
	f.fd = i.fd;
	f.drm_format = i.fourcc;
	f.modifier = i.modifier;
	f.offset = i.offset;
	f.stride = i.stride;
	f.size = i.memory == ipc::memory_opaque_buffer ? size_t(i.size) : size_t(i.stride) * i.height;
	f.opaque_buffer = i.memory == ipc::memory_opaque_buffer;
	f.buffer_id = i.id;
	if (f.opaque_buffer)
	{
		f.av_sw_format = i.fourcc == ipc::fourcc_p010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
		f.uv_offset = i.uv_offset;
		f.buffer_bytes = i.buffer_bytes;
		f.host_visible = i.host_visible;
	}
	return f;
}
} // namespace

vr_sender::vr_sender(stream_channel & video, vr_stream_params p, callbacks c, bool test_pattern) :
        channel(video), params(p), cb(std::move(c)), node(vr_encode_node())
{
	preset = params.nvenc_preset;
	if (test_pattern)
		thread = std::thread(&vr_sender::run_test_pattern, this);
	else
		encoder_thread = std::thread(&vr_sender::run_encoder, this);
}

vr_sender::~vr_sender()
{
	quit = true;
	queue_cv.notify_all();
	if (thread.joinable())
		thread.join();
	if (encoder_thread.joinable())
		encoder_thread.join();
	if (pending && pending->done)
		pending->done();
	if (skipped)
		cb.log(std::format("[vr] {} frames skipped (a newer one arrived while the encoder was busy)", skipped));
	if (held_back)
		cb.log(std::format("[vr] {} frames held back (the network queue was draining)", held_back));
	std::lock_guard lock(encode_mutex);
	if (frames)
		cb.log(std::format("[vr] stream stopped after {} frames", frames));
}

void vr_sender::open_encoders(const std::vector<const dmabuf_image *> & images)
{
	// same codec fallback as the desktop; every stream must agree
	std::vector<video_codec> chain{params.codec};
	if (params.codec == video_codec::av1_10bit)
		chain.push_back(video_codec::av1);
	if (is_av1(params.codec) || params.codec == video_codec::hevc_10bit)
		chain.push_back(video_codec::hevc);
	if (params.codec != video_codec::h264)
		chain.push_back(video_codec::h264);
	// bitrate shares like the Windows streamer: the eyes half each; foveated, each eye's half goes 1/3 to its periphery
	// and 2/3 to its fovea, whatever their sizes
	double weight[4] = {}, total = 0;
	for (size_t k = 0; k < images.size(); ++k)
		total += weight[k] = k >= 2 ? 2 : 1;
	std::string errors;
	for (video_codec c: chain)
	{
		try
		{
			for (size_t k = 0; k < images.size(); ++k)
			{
				int kbps = std::max(int(params.kbps * weight[k] / total), 1000);
				encoder_config ec{int(images[k]->width), int(images[k]->height), params.fps, kbps, c, node, params.full_range};
				ec.source_node = vr_render_node();
				ec.vbaq = params.vbaq;
				ec.pre_encode = params.pre_encode;
				ec.preset = preset;
				ec.amd_preset = params.amd_preset;
				enc[k] = std::make_unique<hardware_encoder>(ec);
				enc[k]->set_log(cb.log);
				enc[k]->prime(frame_of(*images[k]));
			}
			codec = c;
			break;
		}
		catch (const std::exception & ex)
		{
			for (auto & e: enc)
				e.reset();
			errors += std::format(" {}: {};", codec_name(c), ex.what());
		}
	}
	if (!enc[0])
		throw std::runtime_error("no usable hardware encoder:" + errors);
	if (codec != params.codec)
		cb.log(std::format("[vr] {} unavailable, streaming {} instead:{}", codec_name(params.codec), codec_name(codec), errors));
	open_count = images.size();
	if (cb.active_codec)
		cb.active_codec(codec);
	idr = true;
	const dmabuf_image & l = *images[0];
	const bool foveated = images.size() == 4;
	// the same format again (encoders re-opened for another preset): the headset keeps its decoders
	std::string format = std::format("{}/{}/{}x{}/{}", int(codec), params.fps, l.width, l.height,
	                                 foveated ? std::format("{}x{}", images[2]->width, images[2]->height) : std::string());
	if (format == announced_format)
		return;
	announced_format = format;
	if (foveated)
	{
		channel.send(pack_vr_video_format_foveated(codec, params.fps, int(l.width), int(l.height), int(images[2]->width),
		                                           int(images[2]->height), params.eye_width, params.eye_height));
		cb.log(std::format("[vr] -> VideoFormat VR {} foveated x4, periphery {}x{} + fovea {}x{} per eye (eye {}x{}) @{}, {} kbps ({})",
		                   codec_name(codec), l.width, l.height, images[2]->width, images[2]->height, params.eye_width, params.eye_height,
		                   params.fps, params.kbps, enc[0]->input_path()));
		return;
	}
	channel.send(pack_vr_video_format(codec, params.fps, int(l.width), int(l.height), params.eye_width, params.eye_height));
	cb.log(std::format("[vr] -> VideoFormat VR {} x2, encoded {}x{} per eye (eye {}x{}) @{}, {} kbps ({})", codec_name(codec), l.width,
	                   l.height, params.eye_width, params.eye_height, params.fps, params.kbps, enc[0]->input_path()));
}

void vr_sender::encode_and_send(const std::vector<const dmabuf_image *> & images, const vr_render_info & info)
{
	using clock = std::chrono::steady_clock;
	std::lock_guard lock(encode_mutex);
	if (!alive)
		return;
	try
	{
		if (!enc[0] || open_count != images.size() || enc[0]->config().width != int(images[0]->width) ||
		    enc[0]->config().height != int(images[0]->height) ||
		    (images.size() == 4 && enc[2]->config().width != int(images[2]->width)))
			open_encoders(images);
		// a frame not yet out of the socket when the next is due (a Wi-Fi dip, a big keyframe): skip the next one before
		// encoding instead of queueing it. A standing queue (TCP's send buffer grows to several MB) would otherwise add
		// latency that never drains while the bitrate matches the link - until the stream restarts.
		{
			const int64_t frame_bytes = int64_t(params.kbps) * 1000 / 8 / std::max(params.fps, 1);
			const int64_t unsent_limit = std::max<int64_t>(frame_bytes, 64 * 1024);
			const int64_t total_limit = std::max<int64_t>(int64_t(params.kbps) * 1000 / 8 * 30 / 1000, 2 * unsent_limit); // 30 ms
			auto q = channel.queued();
			max_queue = std::max(max_queue, q.total);
			if (frames > 0 && (q.unsent > unsent_limit || q.total > total_limit))
			{
				++held_back;
				return;
			}
		}
		auto t0 = clock::now();
		double pose_age = cb.pose_age_ms ? cb.pose_age_ms(info.pose_index) : -1;
		float game_ms = last_submit.time_since_epoch().count() ? std::chrono::duration<float, std::milli>(t0 - last_submit).count()
		                                                       : 1000.f / float(params.fps);
		last_submit = t0;
		const size_t n = images.size();
		bytes au[4];
		bool sync[4] = {false, false, false, false};
		bool key = idr.exchange(false);
		// Frames from another GPU: one upload at a time. Together they would share the link and both finish last; this
		// way the first image is uploaded in half the time and encodes while the next one uploads.
		std::mutex upload;
		const bool other_gpu = enc[0]->config().source_node != enc[0]->config().render_node;
		auto encode_image = [&](size_t k) {
			const captured_frame frame = frame_of(*images[k]);
			if (other_gpu)
			{
				std::lock_guard lock(upload);
				enc[k]->stage(frame);
			}
			else
				enc[k]->stage(frame);
			enc[k]->reencode_last(key, [&, k](bytes && a, bool s) {
				au[k] = std::move(a);
				sync[k] = s;
			});
		};
		// every image at once: separate encoder sessions (NVENC/VAAPI run them concurrently)
		std::vector<std::future<void>> others;
		for (size_t k = 1; k < n; ++k)
			others.push_back(std::async(std::launch::async, encode_image, k));
		encode_image(0);
		for (auto & f: others)
			f.get(); // rethrows their errors
		double this_encode = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
		encode_ms += this_encode;
		worst_encode = std::max(worst_encode, this_encode);
		adapt_speed(this_encode);
		for (size_t k = 0; k < n; ++k)
			if (au[k].empty())
				return;
		// wire order: s0 left (periphery), s1 right - or foveated: s1 left fovea, s2 right periphery, s3 right fovea
		static constexpr size_t foveated_order[4] = {0, 2, 1, 3}; // stream -> image (images: L, R, Lf, Rf)
		auto image_of = [&](size_t stream) { return n == 4 ? foveated_order[stream] : stream; };
		int64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count();
		bytes packet = pack_vr_frame_header(ts, info, sync[0], params.eye_width, params.eye_height, int32_t(au[0].size()),
		                                    1000.f / float(params.fps), vibration_left, vibration_right);
		// FrameTime: pose arrival -> encode start (the app's rendering + our compositing); a frame period if unknown
		set_frame_timing(packet, pose_age >= 0 && pose_age < 1000 ? float(pose_age) : 1000.f / float(params.fps),
		                 std::min(game_ms, 1000.f), float(this_encode));
		packet.insert(packet.end(), au[0].begin(), au[0].end());
		bool sent = channel.send(packet);
		for (size_t stream = 1; stream < n && sent; ++stream)
		{
			const bytes & a = au[image_of(stream)];
			bytes slice = pack_slice_header(ts, int32_t(stream), int32_t(a.size()));
			slice.insert(slice.end(), a.begin(), a.end());
			sent = channel.send(slice);
		}
		if (!sent)
		{
			alive = false;
			return;
		}
		if (frames == 0)
			cb.log(std::format("[vr] -> first stereo frame ({}) {} + {} bytes", sync[0] ? "sync" : "NON-SYNC", au[0].size(), au[1].size()));
		++frames;
		{
			const auto sent_at = clock::now();
			if (last_sent.time_since_epoch().count())
				longest_gap = std::max(longest_gap, std::chrono::duration<double, std::milli>(sent_at - last_sent).count());
			last_sent = sent_at;
		}
		for (size_t k = 0; k < n; ++k)
			sent_bytes += int64_t(au[k].size());
		const size_t n_images = n;
		if (auto span = std::chrono::duration<double>(clock::now() - window).count(); span >= 5)
		{
			static thread_local int64_t last_frames = 0;
			double n = double(frames - last_frames);
			cb.log(std::format("[vr] {} frames, {:.1f} fps, {:.1f} Mbit/s, encode {:.2f} ms per frame{} (worst {:.1f}), longest gap between frames {:.1f} ms, pose frame {}, network queue up to {} KB{}{}",
			                   frames, n / span, sent_bytes * 8 / span / 1e6, n ? encode_ms / n : 0,
			                   enc[0] && std::string_view(enc[0]->backend()) == "NVENC" ? std::format(" (p{})", preset) : std::string(), worst_encode, longest_gap,
			                   info.pose_index, max_queue / 1024, skipped ? std::format(", {} skipped so far", skipped) : std::string(),
			                   held_back ? std::format(", {} held back for the network so far", held_back) : std::string())
			       + (n_images == 4 ? std::format(", fovea at {},{} / {},{} (follows the gaze)", info.fovea_top_left[0][0], info.fovea_top_left[0][1],
			                                      info.fovea_top_left[1][0], info.fovea_top_left[1][1])
			                        : std::string()));
			if (cb.encode_stats && n > 0)
			{
				double pixels = 0;
				for (size_t k = 0; k < open_count && enc[k]; ++k)
					pixels += double(enc[k]->config().width) * enc[k]->config().height;
				cb.encode_stats(encode_ms / n, worst_encode, params.fps, pixels / 1e6, enc[0] && std::string_view(enc[0]->backend()) == "NVENC");
			}
			max_queue = 0;
			worst_encode = longest_gap = 0;
			last_frames = frames;
			sent_bytes = 0;
			encode_ms = 0;
			window = clock::now();
		}
	}
	catch (const std::exception & e)
	{
		cb.log(std::string("[vr] encoder error: ") + e.what());
		alive = false;
	}
}

void vr_sender::submit(std::vector<std::shared_ptr<const dmabuf_image>> images, const vr_render_info & info, std::function<void()> done)
{
	std::function<void()> dropped;
	{
		std::lock_guard lock(queue_mutex);
		if (pending)
		{
			dropped = std::move(pending->done);
			++skipped;
		}
		pending = job{std::move(images), info, std::move(done)};
	}
	queue_cv.notify_one();
	if (dropped)
		dropped();
}

void vr_sender::run_encoder()
{
	for (;;)
	{
		job j;
		{
			std::unique_lock lock(queue_mutex);
			queue_cv.wait(lock, [&] { return quit || pending; });
			if (quit)
				return;
			j = std::move(*pending);
			pending.reset();
		}
		std::vector<const dmabuf_image *> images;
		for (const auto & i: j.images)
			images.push_back(i.get());
		encode_and_send(images, j.info);
		j.done();
	}
}

void vr_sender::adapt_speed(double ms)
{
	// OPENDESK_TEST_ADAPTIVE_SPEED=1 (harness): any encoder counts, and it is always "too slow" - exercises the re-open
	static const bool test = [] {
		const char * e = std::getenv("OPENDESK_TEST_ADAPTIVE_SPEED");
		return e && *e && *e != '0';
	}();
	if (!params.adaptive_speed || !enc[0] || (!test && std::string_view(enc[0]->backend()) != "NVENC"))
		return;
	load_ms += test ? 1000.0 : ms;
	if (++load_frames < (test ? params.fps / 4 : params.fps)) // about once a second
		return;
	using namespace std::chrono_literals;
	double avg = load_ms / load_frames, budget = 1000.0 / params.fps;
	load_ms = 0;
	load_frames = 0;
	auto now = std::chrono::steady_clock::now();
	int want = preset;
	if (avg > 0.9 * budget && preset > 1)
		want = preset - 1; // can't keep up: faster
	else if (avg < 0.55 * budget && preset < params.nvenc_preset && now - preset_changed > 15s)
		want = preset + 1; // lots of headroom for a while: back towards the default
	if (want == preset)
		return;
	cb.log(std::format("[vr] encoding takes {:.1f} ms of the {:.1f} ms frame budget: NVENC preset p{} -> p{}", avg, budget, preset, want));
	preset = want;
	preset_changed = now;
	for (auto & e: enc) // re-opened with the new preset on the next frame (IDR, same VideoFormat)
		e.reset();
}

void vr_sender::run_test_pattern()
{
	using clock = std::chrono::steady_clock;
	try
	{
		gbm_allocator alloc(node);
		vr_test_pattern pattern(alloc);
		auto left = alloc.allocate(uint32_t(params.encoded_width()), uint32_t(params.encoded_height()));
		auto right = alloc.allocate(uint32_t(params.encoded_width()), uint32_t(params.encoded_height()));
		const auto period = std::chrono::nanoseconds(1000000000LL / std::max(params.fps, 1));
		auto next = clock::now();
		cb.log("[vr] no OpenXR app connected: streaming the test pattern");
		while (!quit && alive)
		{
			vr_render_info info = cb.pose ? cb.pose() : vr_render_info{};
			pattern.render(*left, 0, info.eye[0], info.fov[0]);
			pattern.render(*right, 1, info.eye[1], info.fov[1]);
			encode_and_send({left.get(), right.get()}, info);
			next += period;
			if (auto now = clock::now(); next < now - period)
				next = now; // fell behind: don't burst
			std::this_thread::sleep_until(next);
		}
	}
	catch (const std::exception & e)
	{
		cb.log(std::string("[vr] error: ") + e.what());
		alive = false;
	}
}
} // namespace od
