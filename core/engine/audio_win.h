// Windows audio: WASAPI loopback capture for the headset (desktop audio) and WASAPI render for the
// headset microphone. The microphone is played to a configurable render device; use VB-Cable (or any
// virtual audio cable) to expose it to applications as an input device.
#pragma once

#include "channel.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace od
{
bool send_all(int fd, const void * data, size_t size);

class desktop_audio_sender
{
public:
	// device is ignored on Windows: the default render endpoint is captured in loopback mode.
	desktop_audio_sender(stream_channel &, std::string device, const std::atomic<bool> & enabled);
	~desktop_audio_sender();
	void set_paused(bool p) { paused = p; }

private:
	void run();
	stream_channel & channel;
	std::string device;
	const std::atomic<bool> & enabled;
	std::atomic<bool> paused{false};
	std::atomic<bool> quit{false};
	std::thread thread;
};

class mic_player
{
public:
	explicit mic_player(float mic_volume);
	~mic_player();
	void push(const uint8_t * pcm, size_t size);
	void set_volume(float mic_volume);

private:
	void run();
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<std::vector<uint8_t>> queue;
	bool quit = false;
	std::atomic<float> gain{1.f};
	std::thread thread;
};
} // namespace od
