#ifndef _WIN32 // the Windows transport is in vr_client_win.cpp
#include "vr_client.h"

#include <cerrno>
#include <cstddef>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace od
{
std::unique_ptr<vr_client> vr_client::connect(const std::string & application, std::string * error)
{
	std::unique_ptr<vr_client> c(new vr_client);
	// the path socket, else the abstract one (inside Steam's pressure-vessel the runtime dir isn't shared)
	std::string path = ipc::socket_path(), name = ipc::abstract_name();
	std::string failure;
	for (int attempt = 0; attempt < 2 && c->fd < 0; ++attempt)
	{
		sockaddr_un addr{};
		addr.sun_family = AF_UNIX;
		socklen_t len = sizeof(addr);
		if (attempt == 0)
		{
			if (path.size() >= sizeof(addr.sun_path))
				continue;
			std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
		}
		else
		{
			std::memcpy(addr.sun_path, name.data(), name.size());
			len = socklen_t(offsetof(sockaddr_un, sun_path) + name.size());
		}
		int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr *>(&addr), len) == 0 && ipc::same_user(fd))
			c->fd = fd;
		else
		{
			failure += (failure.empty() ? "" : ", ") + (attempt == 0 ? path : "@" + name.substr(1)) + ": " + strerror(errno);
			if (fd >= 0)
				close(fd);
		}
	}
	if (c->fd < 0)
	{
		if (error)
			*error = "OpenDesk Streamer is not running with VR enabled (" + failure + ")";
		return nullptr;
	}
	ipc::hello_msg h;
	h.pid = getpid();
	std::snprintf(h.application, sizeof(h.application), "%s", application.c_str());
	if (send(c->fd, &h, sizeof(h), MSG_NOSIGNAL) != ssize_t(sizeof(h)))
	{
		if (error)
			*error = "handshake failed";
		return nullptr;
	}
	// the configuration follows right away
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!c->configured() && std::chrono::steady_clock::now() < deadline)
		if (!c->poll(100))
		{
			if (error)
				*error = "the streamer closed the connection";
			return nullptr;
		}
	if (!c->configured())
	{
		if (error)
			*error = "no configuration from the streamer";
		return nullptr;
	}
	return c;
}

vr_client::~vr_client()
{
	drop_config();
	if (state)
		munmap(const_cast<ipc::shared_state *>(state), state_size);
	if (fd >= 0)
		close(fd);
}

void vr_client::drop_config()
{
	for (auto & i: images)
		if (i.fd >= 0)
			close(i.fd);
	images.clear();
	busy.clear();
}

bool vr_client::poll(int timeout_ms)
{
	pollfd p{fd, POLLIN, 0};
	int r = ::poll(&p, 1, timeout_ms);
	if (r < 0)
		return errno == EINTR;
	if (r == 0)
		return true;
	for (;;)
	{
		alignas(8) uint8_t buf[512];
		iovec iov{buf, sizeof(buf)};
		alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * (1 + 4 * ipc::max_slots))];
		msghdr m{};
		m.msg_iov = &iov;
		m.msg_iovlen = 1;
		m.msg_control = control;
		m.msg_controllen = sizeof(control);
		ssize_t n = recvmsg(fd, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (n < 0)
			return errno == EAGAIN || errno == EINTR;
		if (n == 0)
			return false;
		std::vector<int> fds;
		for (cmsghdr * c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
			if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
			{
				size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
				fds.resize(count);
				std::memcpy(fds.data(), CMSG_DATA(c), count * sizeof(int));
			}
		ipc::msg type;
		std::memcpy(&type, buf, sizeof(type));
		if (type == ipc::msg::config && size_t(n) >= sizeof(ipc::config_msg))
		{
			ipc::config_msg c;
			std::memcpy(&c, buf, sizeof(c));
			const size_t per_slot = c.foveated ? 4 : 2;
			if (fds.size() != 1 + per_slot * size_t(c.slot_count))
			{
				for (int f: fds)
					close(f);
				continue;
			}
			drop_config();
			if (!state)
			{
				void * p2 = mmap(nullptr, c.shared_state_size, PROT_READ, MAP_SHARED, fds[0], 0);
				if (p2 != MAP_FAILED)
				{
					state = static_cast<const ipc::shared_state *>(p2);
					state_size = c.shared_state_size;
				}
			}
			close(fds[0]);
			for (size_t i = 1; i < fds.size(); ++i)
				images.push_back({fds[i], (i - 1) % per_slot >= 2 ? c.fovea_image : c.image});
			busy.assign(c.slot_count, false);
			config_ = c;
		}
		else if (type == ipc::msg::release && size_t(n) >= sizeof(ipc::release_msg))
		{
			ipc::release_msg r2;
			std::memcpy(&r2, buf, sizeof(r2));
			if (r2.generation == config_.generation && r2.slot < busy.size())
				busy[r2.slot] = false;
		}
		else
			for (int f: fds)
				close(f);
	}
}

void vr_client::read_state(ipc::shared_state & out) const
{
	if (state)
		ipc::read_shared_state(state, out);
}

std::optional<uint32_t> vr_client::acquire(int timeout_ms)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	for (;;)
	{
		for (uint32_t i = 0; i < busy.size(); ++i)
			if (!busy[i])
				return i;
		auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
		if (left <= 0 || !poll(int(left)))
			return std::nullopt;
	}
}

bool vr_client::submit(uint32_t slot, const vr_render_info & info, int64_t display_time_ns)
{
	if (slot >= busy.size())
		return false;
	reserve(slot);
	return send_submit(config_.generation, slot, info, display_time_ns);
}

void vr_client::reserve(uint32_t slot)
{
	if (slot < busy.size())
		busy[slot] = true;
}

bool vr_client::send_submit(uint32_t generation, uint32_t slot, const vr_render_info & info, int64_t display_time_ns) const
{
	ipc::submit_msg s;
	s.generation = generation;
	s.slot = slot;
	s.info = info;
	s.display_time_ns = display_time_ns;
	return send(fd, &s, sizeof(s), MSG_NOSIGNAL) == ssize_t(sizeof(s));
}

void vr_client::haptics(int hand, float amplitude, float frequency, uint32_t duration_ms)
{
	ipc::haptics_msg h;
	h.hand = uint32_t(hand);
	h.amplitude = amplitude;
	h.frequency = frequency;
	h.duration_ms = duration_ms;
	send(fd, &h, sizeof(h), MSG_NOSIGNAL);
}

void vr_client::render_thread(int tid)
{
	ipc::render_thread_msg m;
	m.tid = tid;
	send(fd, &m, sizeof(m), MSG_NOSIGNAL);
}

void vr_client::recenter()
{
	ipc::recenter_msg r;
	send(fd, &r, sizeof(r), MSG_NOSIGNAL);
}

void vr_client::log(const std::string & text)
{
	ipc::log_msg m;
	std::strncpy(m.text, text.c_str(), sizeof(m.text) - 1);
	send(fd, &m, sizeof(m), MSG_NOSIGNAL);
}

void vr_client::announce_rendering()
{
	ipc::rendering_msg r;
	send(fd, &r, sizeof(r), MSG_NOSIGNAL);
}
} // namespace od
#endif
