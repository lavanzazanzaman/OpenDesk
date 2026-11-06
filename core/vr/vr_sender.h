// VR video: two eye images per frame (defoveation-warped, encoded size), each eye its own encoder, sent as a
// Frame (left) + Slice (right, StreamIndex 1) pair with the same TimestampUs. Frames come from either
//   - the built-in test pattern (own thread, paced at the headset's VR frame rate), or
//   - an OpenXR runtime through vr_server (submit(), called on the server thread when the app submits)
#pragma once

#include "channel.h"
#include "dmabuf.h"
#include "video.h"
#include "video_source.h"
#include "vr_server.h"
#include "vr_types.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <condition_variable>
#include <optional>
#include <thread>

namespace od
{
class hardware_encoder;

struct vr_stream_params
{
	int fps = 72;
	int kbps = 100000; // both eyes together
	video_codec codec = video_codec::hevc;
	int eye_width = 2496, eye_height = 2688; // render size (Bounds)
	bool full_range = false;
	bool vbaq = false, pre_encode = false;
	bool adaptive_speed = false; // NVENC: faster presets while encoding is slower than the frame period
	// foveated streaming (eye-tracked headsets): a fovea crop of this size per eye at render density, periphery at
	// periphery_width x periphery_height (0: off)
	int fovea_width = 0, fovea_height = 0;
	// foveated: the periphery's size as a fraction of the render size (Windows: 1/2). The headset stretches it over the
	// eye whatever its size, so eye-tracked headsets with weaker decoders (Quest Pro) can go lower
	double periphery_scale = 0.5;
	bool foveated() const { return fovea_width > 0 && fovea_height > 0; }
	// encoded eye image: the warped eye at render size / 1.5, or when foveated a coarser periphery

	int encoded_width() const { return int(foveated() ? eye_width * periphery_scale : eye_width / defoveation_ratio) / 16 * 16; }
	int encoded_height() const { return int(foveated() ? eye_height * periphery_scale : eye_height / defoveation_ratio) / 16 * 16; }
};

class vr_sender : public video_source, public vr_server::frame_sink
{
public:
	struct callbacks
	{
		std::function<void(std::string)> log;               // any thread
		std::function<vr_render_info()> pose;               // test pattern: latest headset pose -> eye poses + FOV
		std::function<void(video_codec)> active_codec;      // any thread
		std::function<double(int32_t pose_index)> pose_age_ms; // since that FrameData arrived; < 0 if unknown
	};

	vr_sender(stream_channel & video, vr_stream_params, callbacks, bool test_pattern);
	~vr_sender() override;

	bool running() const override { return alive; }
	void request_keyframe() override { idr = true; }
	void set_vibration(float strong, float weak) override
	{
		vibration_left = strong;
		vibration_right = weak;
	}
	// queues the frame for the encode thread and returns; done() once the encoder is finished with the images. A frame
	// still waiting when the next one arrives is skipped (newest wins: lowest latency)
	void submit(std::vector<std::shared_ptr<const dmabuf_image>> images, const vr_render_info &, std::function<void()> done) override;

private:
	void run_test_pattern();
	void run_encoder();
	void adapt_speed(double encode_ms); // params.adaptive_speed: NVENC preset follows the encoder load
	// images: left, right (+ left fovea, right fovea)
	void open_encoders(const std::vector<const dmabuf_image *> & images); // throws
	void encode_and_send(const std::vector<const dmabuf_image *> & images, const vr_render_info &);

	stream_channel & channel;
	vr_stream_params params;
	callbacks cb;
	std::string node;
	std::mutex encode_mutex;
	std::unique_ptr<hardware_encoder> enc[4];
	size_t open_count = 0;
	video_codec codec{};
	std::atomic<bool> alive{true}, quit{false}, idr{true};
	std::atomic<float> vibration_left{0}, vibration_right{0};
	int64_t frames = 0, sent_bytes = 0;
	double encode_ms = 0;
	std::chrono::steady_clock::time_point last_submit{};
	std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
	std::thread thread;

	struct job
	{
		std::vector<std::shared_ptr<const dmabuf_image>> images;
		vr_render_info info;
		std::function<void()> done;
	};
	std::mutex queue_mutex;
	std::condition_variable queue_cv;
	std::optional<job> pending;
	uint64_t skipped = 0;
	uint64_t held_back = 0; // frames not encoded while the network queue drained
	int max_queue = 0;      // bytes, this stats window
	std::thread encoder_thread;

	int preset = 4; // NVENC p1..p7 (adaptive speed lowers it under load)
	double load_ms = 0;
	int load_frames = 0;
	std::chrono::steady_clock::time_point preset_changed{};
	std::string announced_format; // VideoFormat already sent (re-opening the encoders with the same one skips it)
};
} // namespace od
