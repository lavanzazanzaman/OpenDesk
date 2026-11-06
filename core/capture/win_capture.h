// DXGI Desktop Duplication capture for one monitor, on the GPU: each frame is copied into a texture of our own on the
// monitor's adapter, the cursor drawn into it, and handed to the encoder as a D3D11 texture (captured_frame::d3d11_*),
// which converts it with the video processor - no trip through system memory.
#pragma once

#include "hardware_encoder.h" // captured_frame

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace od
{
class win_capture
{
public:
	using frame_callback = std::function<void(const captured_frame &, int width, int height)>;

	win_capture(std::string device_name, int fps, frame_callback, std::function<void(std::string)> log);
	~win_capture();
	win_capture(const win_capture &) = delete;
	win_capture & operator=(const win_capture &) = delete;

	bool failed() const { return failed_; }

private:
	void run();
	bool open();
	void close();
	void update_cursor_shape();
	void draw_cursor();
	bool cursor_visible = false;
	int cursor_x = 0, cursor_y = 0;

	std::string device;
	int fps;
	frame_callback on_frame;
	std::function<void(std::string)> log;
	std::atomic<bool> quit{false};
	std::atomic<bool> failed_{false};
	struct impl;
	std::unique_ptr<impl> state;
	std::thread thread;
};
} // namespace od
