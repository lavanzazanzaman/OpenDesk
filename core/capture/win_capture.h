// DXGI Desktop Duplication capture for one monitor.
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
	bool grab_pointer(void * mapped, int stride, int width, int height, int x, int y);

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
