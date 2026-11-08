// Desktop video, one or more monitors: a PipeWire capture + VAAPI encoder per streamed monitor, multiplexed on the
// video channel like the Windows DesktopStreamer:
//   stream 0 (main screen)      -> Frame packets (FrameMetadata: timestamps, gamepad vibration)
//   streams 1..3 (extra screens) -> Slice packets tagged with the stream index (the headset runs one decoder each)
// All screens go out in lockstep: after decoding a main frame the headset waits (up to 10 ms each) for a frame from
// every extra decoder, so a change on any screen encodes every screen - repeats of static screens are tiny.
// VideoFormat announces every stream's frame size and desktop Bounds. The headset places the extra screens around
// the main one from the bounds, and its laser pointers come back in the same coordinate space.
#pragma once

#include "channel.h"
#include "display_info.h"
#include "video.h"
#include "video_source.h"

#include <atomic>
#include <condition_variable>
#include <thread>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace od
{
class pipewire_capture;
class x11_capture;
class hardware_encoder;
struct captured_frame;

struct desktop_stream_setup
{
	int pipewire_fd = -1;  // portal remote (owned by the sender); -1: the session's PipeWire daemon (KWin)
	uint32_t node_id = 0;
	uint64_t serial = 0;
	std::string name;      // for logs ("DP-1")
	rect bounds;           // desktop coordinates, see VideoFormat.Bounds
	int expected_width = 0, expected_height = 0; // pixels; announced until the first frame tells the real size
	bool x11 = false;      // X11 session: grabbed from the root window at x11_x, x11_y (expected size)
	int x11_x = 0, x11_y = 0;
	int monitor_index = -1;
};

class desktop_video_sender : public video_source
{
public:
	struct callbacks
	{
		std::function<void(std::string)> log;          // any thread
		std::function<void(video_codec)> active_codec; // any thread: codec actually in use (after fallbacks)
	};

	// throws if a capture stream cannot be set up
	desktop_video_sender(stream_channel & video_channel, desktop_video_params, std::vector<desktop_stream_setup>, callbacks);
	~desktop_video_sender() override;

	bool running() const override;
	void request_keyframe() override;
	void set_vibration(float strong, float weak) override;

private:
	struct stream_state;
	void on_frame(stream_state &, const captured_frame &, int width, int height);
	void open_encoder(stream_state &, const captured_frame &, int width, int height);
	void send_au(stream_state &, bytes && au, bool sync);
	void wake();                           // schedule an encode pass
	void encode_loop();                    // one pass = every screen, main first

	stream_channel & channel;
	desktop_video_params params;
	callbacks cb;
	std::atomic<bool> alive{true};

	std::mutex format_mutex; // codec choice + announced VideoFormat
	std::optional<video_codec> active_codec;
	std::vector<stream_format> announced; // per screen, as known
	std::vector<stream_format> last_sent;
	std::atomic<bool> format_sent{false};

	std::mutex socket_mutex; // frame index + stats; packets of different screens stay whole via the channel
	bool socket_ok = true;
	int32_t frame_index = 0;
	std::atomic<float> vibration_left{0}, vibration_right{0};
	uint64_t window_frames = 0, window_bytes = 0;
	int64_t window_start_us = 0;

	std::mutex wake_mutex;
	std::condition_variable wake_cv;
	bool wake_pending = false;
	std::thread encoder_thread;

	std::vector<std::unique_ptr<stream_state>> streams; // last: captures stop before everything above goes away
};
} // namespace od
