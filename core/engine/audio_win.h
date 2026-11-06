// Windows audio: WASAPI loopback capture for the headset (desktop audio) and WASAPI render for the
// headset microphone. Windows has no virtual microphone without a driver: the microphone is played into a virtual
// audio cable (VB-CABLE's "CABLE Input", or OPENDESK_MIC_RENDER_DEVICE), which apps record from as "CABLE Output".
// Without one it isn't played anywhere (never on the PC's speakers).
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
bool virtual_mic_available();     // the virtual cable the headset microphone is played into exists
std::string virtual_mic_device(); // its name

class desktop_audio_sender
{
public:
	// the default render endpoint is captured in loopback mode; device "mute-speakers" ("VR headset only") also mutes
	// it while streaming
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
