#include "audio.h"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <poll.h>
#include <sys/socket.h>

namespace od
{
namespace
{
constexpr uint32_t rate = 48000;
constexpr size_t packet_ms = 5;
constexpr size_t packet_bytes = rate * packet_ms / 1000 * 2 * 2; // 960: 5 ms stereo s16

} // namespace

bool send_all(int fd, const void * data, size_t size)
{
	auto * p = static_cast<const uint8_t *>(data);
	while (size)
	{
		ssize_t n = ::send(fd, p, size, MSG_NOSIGNAL);
		if (n > 0)
		{
			p += n;
			size -= size_t(n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		{
			pollfd pfd{fd, POLLOUT, 0};
			if (poll(&pfd, 1, 1000) < 0 || (pfd.revents & (POLLERR | POLLHUP)))
				return false;
			continue;
		}
		return false;
	}
	return true;
}

desktop_audio_sender::desktop_audio_sender(stream_channel & c, std::string device_, const std::atomic<bool> & enabled_) :
        channel(c), device(std::move(device_)), enabled(enabled_), thread(&desktop_audio_sender::run, this) {}

desktop_audio_sender::~desktop_audio_sender()
{
	quit = true;
	thread.join();
}

void desktop_audio_sender::run()
{
	pa_sample_spec spec{PA_SAMPLE_S16LE, rate, 2};
	// small fragments: the server delivers every 5 ms instead of its default ~20-40 ms
	pa_buffer_attr attr{uint32_t(-1), uint32_t(-1), uint32_t(-1), uint32_t(-1), uint32_t(packet_bytes)};
	int err = 0;
	pa_simple * pa = pa_simple_new(nullptr, "OpenDesk Streamer", PA_STREAM_RECORD, device.c_str(),
	                               "desktop audio -> headset", &spec, nullptr, &attr, &err);
	if (!pa)
	{
		fprintf(stderr, "audio capture failed: %s\n", pa_strerror(err));
		return;
	}
	std::vector<uint8_t> packet(4 + packet_bytes);
	*reinterpret_cast<int32_t *>(packet.data()) = int32_t(packet_bytes);
	while (!quit)
	{
		// keep capturing while disabled so re-enabling is instant and nothing stale queues up
		if (pa_simple_read(pa, packet.data() + 4, packet_bytes, &err) < 0)
			break;
		if (!enabled || paused)
			continue;
		if (!channel.send(packet.data(), packet.size()))
			break;
	}
	pa_simple_free(pa);
}

mic_player::mic_player(float mic_volume) :
        thread(&mic_player::run, this)
{
	set_volume(mic_volume);
}

void mic_player::set_volume(float v)
{
	gain = 0.1f + (2.f - 0.1f) * std::clamp(v, 0.f, 1.f);
}

mic_player::~mic_player()
{
	{
		std::lock_guard lock(mutex);
		quit = true;
	}
	cv.notify_all();
	thread.join();
}

void mic_player::push(const uint8_t * pcm, size_t size)
{
	{
		std::lock_guard lock(mutex);
		if (queue.size() > 50) // ~0.5 s backlog: drop rather than lag
			queue.clear();
		queue.emplace_back(pcm, pcm + size);
	}
	cv.notify_one();
}

void mic_player::run()
{
	pa_simple * pa = nullptr;
	for (;;)
	{
		std::vector<uint8_t> chunk;
		{
			std::unique_lock lock(mutex);
			cv.wait(lock, [&] { return quit || !queue.empty(); });
			if (quit)
				break;
			chunk = std::move(queue.front());
			queue.pop_front();
		}
		if (!pa)
		{
			pa_sample_spec spec{PA_SAMPLE_S16LE, rate, 1};
			int err = 0;
			pa = pa_simple_new(nullptr, "OpenDesk Streamer", PA_STREAM_PLAYBACK, "opendesk_mic", "headset microphone",
			                   &spec, nullptr, nullptr, &err);
			if (!pa)
			{
				fprintf(stderr, "mic playback failed: %s\n", pa_strerror(err));
				continue;
			}
		}
		if (float g = gain; std::abs(g - 1.f) > 0.01f)
		{
			auto * s16 = reinterpret_cast<int16_t *>(chunk.data());
			for (size_t i = 0; i < chunk.size() / 2; ++i)
				s16[i] = int16_t(std::clamp(std::lround(s16[i] * g), -32768L, 32767L));
		}
		int err = 0;
		pa_simple_write(pa, chunk.data(), chunk.size(), &err);
	}
	if (pa)
		pa_simple_free(pa);
}
} // namespace od
