#include "channel.h"

#ifdef _WIN32
#include "audio_win.h" // send_all
#else
#include "audio.h" // send_all

#include <linux/sockios.h>
#include <sys/ioctl.h>
#endif

namespace od
{
stream_channel::stream_channel(int socket_fd, std::optional<std::pair<aes_key, aes_iv>> encryption) :
        fd(socket_fd), writer(encryption ? std::make_unique<aes_stream_writer>(encryption->first, encryption->second) : nullptr)
{
}

bool stream_channel::send(const uint8_t * data, size_t size)
{
	std::lock_guard lock(mutex);
	if (!good)
		return false;
	bool sent;
	if (writer)
	{
		bytes chunk = writer->encode(data, size);
		sent = send_all(fd, chunk.data(), chunk.size());
	}
	else
		sent = send_all(fd, data, size);
	if (!sent)
		good = false;
	return sent;
}

stream_channel::queue stream_channel::queued() const
{
#ifdef _WIN32
	return {}; // no SIOCOUTQ on Winsock; only the VR sender (Linux) uses this
#else
	queue q;
	if (ioctl(fd, SIOCOUTQ, &q.total) < 0 || ioctl(fd, SIOCOUTQNSD, &q.unsent) < 0)
		return {};
	return q;
#endif
}
} // namespace od
