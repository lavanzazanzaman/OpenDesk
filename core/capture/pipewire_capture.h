// PipeWire video stream from a screen-cast node (portal or KWin). Offers DMA-BUF (linear) and memory buffers; asks the compositor for
// at most `max_fps` so a 240 Hz desktop isn't encoded at 240 fps. Frames are delivered on PipeWire's thread and
// must be consumed before the callback returns (the buffer goes back to the compositor afterwards).
#pragma once

#include "hardware_encoder.h"

#include <atomic>
#include <functional>
#include <string>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_hook;
struct spa_pod;

namespace od
{
class pipewire_capture
{
public:
	struct format
	{
		int width = 0, height = 0;
		uint32_t drm_format = 0;
		int av_sw_format = -1;
		uint64_t modifier = 0;
		bool dmabuf = false;
	};
	using frame_callback = std::function<void(const captured_frame &, const format &)>;
	using log_callback = std::function<void(std::string)>;

	// takes ownership of pipewire_fd (portal remote); -1 = the session's PipeWire daemon (KWin screen cast).
	// serial: PipeWire object serial of the node when known (preferred: node ids get reused)
	pipewire_capture(int pipewire_fd, uint32_t node_id, uint64_t serial, int max_fps, frame_callback, log_callback);
	~pipewire_capture();
	pipewire_capture(const pipewire_capture &) = delete;
	pipewire_capture & operator=(const pipewire_capture &) = delete;

	// the stream ended (e.g. the captured monitor was switched off / unplugged, or sharing was revoked)
	bool failed() const { return error; }

private:
	static void on_param_changed(void *, uint32_t id, const ::spa_pod *);
	static void on_process(void *);
	static void on_state_changed(void *, int old_state, int state, const char * err);

	pw_thread_loop * loop = nullptr;
	pw_context * context = nullptr;
	pw_core * core = nullptr;
	pw_stream * stream = nullptr;
	spa_hook * listener = nullptr;
	format fmt;
	frame_callback on_frame;
	log_callback log;
	std::atomic<bool> error{false};
};
} // namespace od
