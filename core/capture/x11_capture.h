// X11 sessions (no screen-cast portal there): each streamed monitor grabbed from the root window with MIT-SHM at up to
// `max_fps`, the cursor drawn in (XFixes), delivered as CPU frames (BGR0) on the capture thread - like a
// pipewire_capture memory stream. The monitors come from XRandR (x11_monitors).
#pragma once

#include "hardware_encoder.h"
#include "kwin_screencast.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace od
{
bool x11_session();                       // an X11 desktop (XDG_SESSION_TYPE=x11, or DISPLAY without WAYLAND_DISPLAY)
std::vector<display_info> x11_monitors(); // XRandR monitors, primary first

class x11_capture
{
public:
	using frame_callback = std::function<void(const captured_frame &, int width, int height)>;
	using log_callback = std::function<void(std::string)>;
	x11_capture(int x, int y, int width, int height, int max_fps, frame_callback, log_callback);
	~x11_capture();
	x11_capture(const x11_capture &) = delete;
	x11_capture & operator=(const x11_capture &) = delete;
	bool failed() const { return error; }

private:
	void run();
	int x, y, width, height, fps;
	frame_callback on_frame;
	log_callback log;
	std::atomic<bool> quit{false}, error{false};
	std::thread thread;
};
} // namespace od
