// Windows transport of vr_client (vr_client.cpp is the Linux one): the streamer's per-user message pipe, the tracking
// section and stream buffers as NT handles from the ipc::handle_block that follows each config message.
#ifdef _WIN32
#include "vr_client.h"

#include <windows.h>

#include <chrono>
#include <cstring>

namespace od
{
namespace
{
// one message write; the pipe handle is overlapped (reads wait with a timeout), so writes wait for themselves
bool write_message(HANDLE pipe, const void * data, size_t size)
{
	if (!pipe)
		return false;
	OVERLAPPED ov{};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	DWORD written = 0;
	BOOL ok = WriteFile(pipe, data, DWORD(size), &written, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING)
		ok = GetOverlappedResult(pipe, &ov, &written, TRUE);
	CloseHandle(ov.hEvent);
	return ok && written == size;
}

HANDLE as_handle(native_handle h)
{
	return static_cast<HANDLE>(h);
}
} // namespace

struct vr_client::win32_io
{
	OVERLAPPED ov{};
	alignas(8) uint8_t buffer[sizeof(ipc::config_msg) + sizeof(ipc::handle_block) + 64];
	bool pending = false;
	std::mutex write_mutex; // send_submit may come from another thread
};

std::unique_ptr<vr_client> vr_client::connect(const std::string & application, std::string * error)
{
	std::unique_ptr<vr_client> c(new vr_client);
	c->io = std::make_unique<win32_io>();
	c->io->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	const std::wstring name = ipc::pipe_name();
	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 20 && pipe == INVALID_HANDLE_VALUE; ++attempt)
	{
		pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (pipe != INVALID_HANDLE_VALUE)
			break;
		if (GetLastError() != ERROR_PIPE_BUSY) // every instance taken for a moment: the server creates the next one
			break;
		WaitNamedPipeW(name.c_str(), 100);
	}
	if (pipe == INVALID_HANDLE_VALUE)
	{
		if (error)
			*error = "OpenDesk Streamer is not running with VR enabled (no VR pipe)";
		return nullptr;
	}
	DWORD mode = PIPE_READMODE_MESSAGE;
	SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
	c->fd = pipe;
	ipc::hello_msg h;
	h.pid = int32_t(GetCurrentProcessId());
	std::snprintf(h.application, sizeof(h.application), "%s", application.c_str());
	if (!write_message(pipe, &h, sizeof(h)))
	{
		if (error)
			*error = "handshake failed";
		return nullptr;
	}
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
	if (io && io->pending && fd)
	{
		CancelIoEx(as_handle(fd), &io->ov);
		DWORD n;
		GetOverlappedResult(as_handle(fd), &io->ov, &n, TRUE);
	}
	if (state)
		UnmapViewOfFile(state);
	if (fd)
		CloseHandle(as_handle(fd));
	if (io)
		CloseHandle(io->ov.hEvent);
}

void vr_client::drop_config()
{
	for (auto & i: images)
		if (i.fd)
			CloseHandle(as_handle(i.fd));
	images.clear();
	busy.clear();
}

bool vr_client::poll(int timeout_ms)
{
	HANDLE pipe = as_handle(fd);
	for (bool waited = false;;)
	{
		if (!io->pending)
		{
			ResetEvent(io->ov.hEvent);
			DWORD n = 0;
			const BOOL ok = ReadFile(pipe, io->buffer, sizeof(io->buffer), &n, &io->ov);
			const DWORD e = ok ? 0 : GetLastError();
			if (!ok && e != ERROR_IO_PENDING && e != ERROR_MORE_DATA)
				return false; // broken pipe: the streamer went away
			io->pending = true;
		}
		DWORD wait = waited ? 0 : DWORD(timeout_ms < 0 ? INFINITE : timeout_ms);
		DWORD n = 0;
		if (!GetOverlappedResultEx(pipe, &io->ov, &n, wait, FALSE))
		{
			const DWORD e = GetLastError();
			if (e == WAIT_TIMEOUT || e == ERROR_IO_INCOMPLETE)
				return true; // nothing (more) for now; the read stays pending
			io->pending = false;
			if (e == ERROR_MORE_DATA)
				continue; // not ours: skip the fragment
			return false;
		}
		io->pending = false;
		waited = true;
		const uint8_t * buf = io->buffer;
		ipc::msg type;
		if (n < sizeof(type))
			continue;
		std::memcpy(&type, buf, sizeof(type));
		if (type == ipc::msg::config && n >= sizeof(ipc::config_msg) + sizeof(ipc::handle_block))
		{
			ipc::config_msg c;
			ipc::handle_block block;
			std::memcpy(&c, buf, sizeof(c));
			std::memcpy(&block, buf + sizeof(c), sizeof(block));
			const size_t per_slot = c.foveated ? 4 : 2;
			if (block.count != 1 + per_slot * size_t(c.slot_count) || block.count > std::size(block.values))
				continue;
			// the streamer could not open us (we run elevated): duplicate its handles ourselves
			std::vector<HANDLE> handles(block.count);
			HANDLE owner = block.owner_pid ? OpenProcess(PROCESS_DUP_HANDLE, FALSE, block.owner_pid) : nullptr;
			bool ok = !block.owner_pid || owner;
			for (uint32_t i = 0; i < block.count; ++i)
			{
				HANDLE h = reinterpret_cast<HANDLE>(uintptr_t(block.values[i]));
				if (owner)
				{
					HANDLE mine = nullptr;
					const DWORD access = i == 0 ? FILE_MAP_READ : 0;
					if (!DuplicateHandle(owner, h, GetCurrentProcess(), &mine, access, FALSE, access ? 0 : DUPLICATE_SAME_ACCESS))
						ok = false;
					h = mine;
				}
				handles[i] = h;
			}
			if (owner)
				CloseHandle(owner);
			if (!ok)
			{
				for (HANDLE h: handles)
					if (h && block.owner_pid)
						CloseHandle(h);
				continue;
			}
			drop_config();
			if (!state)
			{
				if (void * view = MapViewOfFile(handles[0], FILE_MAP_READ, 0, 0, c.shared_state_size))
				{
					state = static_cast<const ipc::shared_state *>(view);
					state_size = c.shared_state_size;
				}
			}
			CloseHandle(handles[0]);
			for (size_t i = 1; i < handles.size(); ++i)
				images.push_back({handles[i], (i - 1) % per_slot >= 2 ? c.fovea_image : c.image});
			busy.assign(c.slot_count, false);
			config_ = c;
		}
		else if (type == ipc::msg::release && n >= sizeof(ipc::release_msg))
		{
			ipc::release_msg r;
			std::memcpy(&r, buf, sizeof(r));
			if (r.generation == config_.generation && r.slot < busy.size())
				busy[r.slot] = false;
		}
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

bool vr_client::send_message(const void * data, size_t size) const
{
	std::lock_guard lock(io->write_mutex);
	return write_message(as_handle(fd), data, size);
}

bool vr_client::send_submit(uint32_t generation, uint32_t slot, const vr_render_info & info, int64_t display_time_ns) const
{
	ipc::submit_msg s;
	s.generation = generation;
	s.slot = slot;
	s.info = info;
	s.display_time_ns = display_time_ns;
	return send_message(&s, sizeof(s));
}

void vr_client::haptics(int hand, float amplitude, float frequency, uint32_t duration_ms)
{
	ipc::haptics_msg h;
	h.hand = uint32_t(hand);
	h.amplitude = amplitude;
	h.frequency = frequency;
	h.duration_ms = duration_ms;
	send_message(&h, sizeof(h));
}

void vr_client::render_thread(int tid)
{
	ipc::render_thread_msg m;
	m.tid = tid;
	send_message(&m, sizeof(m));
}

void vr_client::recenter()
{
	ipc::recenter_msg r;
	send_message(&r, sizeof(r));
}

void vr_client::log(const std::string & text)
{
	ipc::log_msg m;
	std::strncpy(m.text, text.c_str(), sizeof(m.text) - 1);
	send_message(&m, sizeof(m));
}

void vr_client::announce_rendering()
{
	ipc::rendering_msg r;
	send_message(&r, sizeof(r));
}
} // namespace od
#endif
