// One streamer -> headset TCP channel (video tcp/38830, audio tcp/38840): the socket plus, with EncryptLocalTraffic,
// its AesStream writer. AesStream is a single CBC chain per direction, so everything written to a channel (desktop
// capture, video files, restarted senders) must go through the same writer: one object per connection.
#pragma once

#include "bytes.h"
#include "crypto.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace od
{
class stream_channel
{
public:
	stream_channel(int socket_fd, std::optional<std::pair<aes_key, aes_iv>> encryption);
	// thread-safe; one call = one packet (one AesStream chunk when encrypted). false once the peer is gone.
	bool send(const uint8_t * data, size_t size);
	bool send(const bytes & b) { return send(b.data(), b.size()); }
	bool ok() const { return good; }
	// bytes in the kernel's send queue: total (sent but not acknowledged + not sent yet), and not sent yet
	struct queue
	{
		int total = 0, unsent = 0;
	};
	queue queued() const;

private:
	int fd;
	std::unique_ptr<aes_stream_writer> writer;
	std::mutex mutex;
	std::atomic<bool> good{true};
};
} // namespace od
