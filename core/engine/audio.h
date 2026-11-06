// Audio channel (tcp/38840), NetAudioClient: packets are [i32 LE size][raw PCM].
//   streamer -> headset: s16le 48 kHz stereo desktop audio (5 ms packets, own thread, direct socket writes)
//   headset -> streamer: s16le 48 kHz mono microphone -> PipeWire virtual source "opendesk_mic"
#pragma once

#include "channel.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace od
{
// Writes the whole buffer to a (non-blocking) socket fd from any thread; false once the peer is gone.
bool send_all(int fd, const void * data, size_t size);

class desktop_audio_sender
{
public:
	desktop_audio_sender(stream_channel &, std::string device, const std::atomic<bool> & enabled);
	~desktop_audio_sender();
	// a video file is playing: its own audio goes to the headset instead
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
	// plays into the PipeWire source "opendesk_mic" (created by the engine); gain = lerp(0.1, 2, MicVolume) as on Windows
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
