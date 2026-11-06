// Windows transport of vr_server (vr_server.cpp is the Linux one; vr_server_state.cpp is shared):
//   - one message-mode named pipe per user (ipc::pipe_name), its DACL limited to this user and SYSTEM, several instances
//     so probing clients can come and go next to the rendering app;
//   - the tracking state in an unnamed file mapping, the stream buffers as exportable Vulkan memory (OPAQUE_WIN32, on
//     every GPU), both duplicated into the client as NT handles that follow the config message (ipc::handle_block);
//   - all pipe I/O on the server thread: overlapped reads, one wait over every connection plus a wake event.
#ifdef _WIN32
#include "vr_server.h"
#include "win_gpu.h"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <stdexcept>

namespace od
{
namespace
{
std::string narrow(const std::wstring & w)
{
	if (w.empty())
		return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

std::string last_error_text(DWORD e = GetLastError())
{
	char * text = nullptr;
	FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
	               reinterpret_cast<char *>(&text), 0, nullptr);
	std::string s = text ? text : std::format("error {}", e);
	if (text)
		LocalFree(text);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.'))
		s.pop_back();
	return s;
}

// the current user (and SYSTEM) only: other users on the machine must not read our tracking data or write into the
// stream buffers
struct pipe_security
{
	SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
	pipe_security()
	{
		std::wstring sid;
		HANDLE token = nullptr;
		if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		{
			DWORD size = 0;
			GetTokenInformation(token, TokenUser, nullptr, 0, &size);
			std::string buffer(size, '\0');
			LPWSTR text = nullptr;
			if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
			    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text))
			{
				sid = text;
				LocalFree(text);
			}
			CloseHandle(token);
		}
		if (sid.empty())
			throw std::runtime_error("cannot determine the current user");
		// protected DACL: full access for the user and SYSTEM; low-integrity callers rejected (mandatory label)
		const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;SY)S:(ML;;NWNR;;;ME)";
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &attributes.lpSecurityDescriptor,
		                                                          nullptr))
			throw std::runtime_error("cannot build the pipe's security descriptor: " + last_error_text());
	}
	~pipe_security()
	{
		LocalFree(attributes.lpSecurityDescriptor);
	}
};

constexpr DWORD pipe_buffer = 4096;
constexpr int max_peers = 8;

// one synchronous message write on an overlapped handle (server thread only)
bool write_message(HANDLE pipe, const void * data, size_t size)
{
	OVERLAPPED ov{};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	DWORD written = 0;
	BOOL ok = WriteFile(pipe, data, DWORD(size), &written, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING)
	{
		// a client that stops reading must not stall the server forever: its pipe buffer holds many small messages
		if (WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0)
		{
			CancelIoEx(pipe, &ov);
			GetOverlappedResult(pipe, &ov, &written, TRUE);
			CloseHandle(ov.hEvent);
			return false;
		}
		ok = GetOverlappedResult(pipe, &ov, &written, FALSE);
	}
	CloseHandle(ov.hEvent);
	return ok && written == size;
}
} // namespace

struct vr_server::win32
{
	pipe_security security;
	std::wstring name;
	HANDLE wake_event = nullptr;
	HANDLE mapping = nullptr;

	// the instance waiting for the next client
	HANDLE listening = nullptr;
	OVERLAPPED connect_ov{};
	bool connect_pending = false;

	struct peer
	{
		HANDLE pipe = nullptr;
		HANDLE process = nullptr; // PROCESS_DUP_HANDLE on the client, when we may open it
		OVERLAPPED read_ov{};
		alignas(8) uint8_t buffer[sizeof(ipc::log_msg) + 64];
		bool reading = false;
		bool greeted = false; // hello received
		std::chrono::steady_clock::time_point connected = std::chrono::steady_clock::now();
		std::string app;
		int pid = 0;
		std::vector<image_ptr> pool;
		int log_lines = 0;
		uint64_t serial = 0;
	};
	std::map<int, peer> peers; // by connection id (vr_server::client_fd on Windows)
	int next_id = 1;
	uint64_t next_serial = 0;

	struct release_queue
	{
		struct item { int id; uint64_t serial; uint32_t generation, slot; };
		std::mutex m;
		std::vector<item> items;
		HANDLE wake = nullptr;
		bool alive = true;
	};
	std::shared_ptr<release_queue> releases = std::make_shared<release_queue>();
	std::mutex wake_mutex;
	std::string wake_reasons; // 'c' configuration, 'q' quit, 'r' releases

	void close_peer(peer & p)
	{
		if (p.reading)
		{
			CancelIoEx(p.pipe, &p.read_ov);
			DWORD n;
			GetOverlappedResult(p.pipe, &p.read_ov, &n, TRUE);
		}
		DisconnectNamedPipe(p.pipe);
		CloseHandle(p.pipe);
		CloseHandle(p.read_ov.hEvent);
		if (p.process)
			CloseHandle(p.process);
	}
};

vr_server::vr_server(const std::string & node, callbacks c) :
        render_node(node.empty() ? win::vr_node() : node), cb(std::move(c)), w(std::make_unique<win32>())
{
	// Windows has no GBM: the stream buffers are always exportable Vulkan memory (NVENC imports it through CUDA, other
	// encoders read it back)
	vk_pool = std::make_unique<vk_buffer_pool>(render_node);
	w->name = ipc::pipe_name();

	w->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(sizeof(ipc::shared_state)), nullptr);
	if (!w->mapping)
		throw std::runtime_error("CreateFileMapping failed: " + last_error_text());
	void * p = MapViewOfFile(w->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::shared_state));
	if (!p)
		throw std::runtime_error("MapViewOfFile failed: " + last_error_text());
	state = new (p) ipc::shared_state{};
	state->version = ipc::protocol_version;
	state->ipd = 0.063f;
	state->frame_period_ns = 1000000000 / 72;

	w->wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	w->releases->wake = w->wake_event;
	w->connect_ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	// FILE_FLAG_FIRST_PIPE_INSTANCE: a second streamer (or a squatter) must not share the name
	w->listening = CreateNamedPipeW(w->name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
	                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
	                                pipe_buffer, pipe_buffer, 0, &w->security.attributes);
	if (w->listening == INVALID_HANDLE_VALUE)
		throw std::runtime_error("cannot create the VR pipe " + narrow(w->name) + ": " + last_error_text());
	// default geometry (Quest 3, Low) until a headset tells us better: apps can start before the headset connects
	set_stream(1728, 1824, 1152, 1216, 72);
	thread = std::thread(&vr_server::run, this);
}

vr_server::~vr_server()
{
	{
		std::lock_guard lock(w->releases->m); // late done() calls from an encoder become no-ops
		w->releases->alive = false;
	}
	quit = true;
	wake('q');
	if (thread.joinable())
		thread.join();
	if (w->listening && w->listening != INVALID_HANDLE_VALUE)
	{
		if (w->connect_pending)
		{
			CancelIoEx(w->listening, &w->connect_ov);
			DWORD n;
			GetOverlappedResult(w->listening, &w->connect_ov, &n, TRUE);
		}
		CloseHandle(w->listening);
	}
	CloseHandle(w->connect_ov.hEvent);
	CloseHandle(w->wake_event);
	if (state)
		UnmapViewOfFile(state);
	CloseHandle(w->mapping);
}

void vr_server::wake(char reason)
{
	{
		std::lock_guard lock(w->wake_mutex);
		w->wake_reasons += reason;
	}
	SetEvent(w->wake_event);
}

void vr_server::run()
{
	using peer = win32::peer;
	auto & peers = w->peers;

	auto select = [&](int id) {
		client_fd = id;
		submitted = false;
		peer_pid = id >= 0 ? peers.at(id).pid : 0;
		if (id >= 0 && cb.client)
			cb.client(true, peers.at(id).app);
	};
	auto disconnect = [&](int id) {
		auto it = peers.find(id);
		if (it == peers.end())
			return;
		const bool selected = client_fd.load() == id;
		const bool greeted = it->second.greeted;
		const auto app = it->second.app;
		{
			std::lock_guard lock(mutex); // the pool may be in the sink's hands; shared_ptrs keep it alive there
			w->close_peer(it->second);
			peers.erase(it);
		}
		if (greeted && cb.log)
			cb.log("[vr-ipc] OpenXR app disconnected: " + app);
		if (selected)
		{
			client_fd = -1;
			submitted = false;
			peer_pid = 0;
			if (cb.client)
				cb.client(false, app);
			for (auto & [other, p]: peers)
				if (p.greeted)
				{
					select(other);
					break;
				}
		}
	};
	// duplicates our handle into the client (or leaves it to the client, see ipc::handle_block)
	auto give = [&](peer & p, HANDLE h, DWORD access, ipc::handle_block & block) -> bool {
		HANDLE out = nullptr;
		if (p.process)
		{
			if (!DuplicateHandle(GetCurrentProcess(), h, p.process, &out, access, FALSE, access ? 0 : DUPLICATE_SAME_ACCESS))
				return false;
		}
		else
			out = h;
		block.values[block.count++] = uint64_t(reinterpret_cast<uintptr_t>(out));
		return true;
	};
	auto send_config = [&](int id) {
		auto & p = peers.at(id);
		std::lock_guard lock(mutex);
		p.pool.clear();
		const bool foveated = fovea_w > 0 && fovea_h > 0;
		const uint32_t per_slot = foveated ? 4 : 2;
		for (uint32_t slot = 0; slot < 3; ++slot) // per slot: left, right (+ left fovea, right fovea)
			for (uint32_t k = 0; k < per_slot; ++k)
				p.pool.push_back(k < 2 ? image_ptr(vk_pool->allocate(uint32_t(enc_w), uint32_t(enc_h), ten_bit))
				                       : image_ptr(vk_pool->allocate(uint32_t(fovea_w), uint32_t(fovea_h), ten_bit)));
		struct
		{
			ipc::config_msg config;
			ipc::handle_block handles;
		} m{};
		auto & c = m.config;
		c = ipc::config_msg{};
		c.generation = generation;
		c.eye_width = uint32_t(eye_w);
		c.eye_height = uint32_t(eye_h);
		c.fps = uint32_t(fps);
		c.slot_count = uint32_t(p.pool.size() / per_slot);
		const dmabuf_image & i = *p.pool[0];
		c.image = {i.width, i.height, i.stride, i.offset, i.fourcc, i.modifier};
		c.image.memory = i.memory;
		c.image.size = i.size;
		c.image.uv_offset = i.uv_offset;
		c.image.buffer_bytes = i.buffer_bytes;
		c.image.full_range = full_range;
		std::memcpy(c.image.device_uuid, vk_pool->device_uuid().data(), 16);
		std::memcpy(c.image.driver_uuid, vk_pool->driver_uuid().data(), 16);
		if (foveated)
		{
			const dmabuf_image & f = *p.pool[2];
			c.foveated = 1;
			c.fovea_image = c.image;
			c.fovea_image.width = f.width;
			c.fovea_image.height = f.height;
			c.fovea_image.stride = f.stride;
			c.fovea_image.uv_offset = f.uv_offset;
			c.fovea_image.size = f.size;
			c.fovea_image.buffer_bytes = f.buffer_bytes;
			if (cb.log)
				cb.log(std::format("[vr-ipc] foveated: {}x{} fovea per eye", f.width, f.height));
		}
		c.shared_state_size = sizeof(ipc::shared_state);
		if (auto a = win::adapter(render_node))
		{
			c.gpu_vendor = a->vendor;
			c.gpu_device = a->device;
			// Windows has no render node numbers: the runtime finds the VR GPU (ovr_Create's LUID, Vulkan) by LUID
			c.render_major = uint32_t(a->luid);
			c.render_minor = uint32_t(a->luid >> 32);
		}
		m.handles.owner_pid = p.process ? 0 : GetCurrentProcessId();
		bool ok = give(p, w->mapping, FILE_MAP_READ, m.handles); // read-only view for the app
		for (const auto & img: p.pool)
			ok = ok && give(p, img->fd, 0, m.handles);
		if (!ok)
			throw std::runtime_error("cannot share the stream buffers with the app: " + last_error_text());
		if (cb.log)
			cb.log(std::format("[vr-ipc] {} buffers {}x{} on {}, Vulkan memory (OPAQUE_WIN32), {} {} range", p.pool.size(), i.width,
			                   i.height, render_node, ten_bit ? "P010" : "NV12", full_range ? "full" : "limited"));
		if (!write_message(p.pipe, &m, sizeof(m)))
			throw std::runtime_error("sending the configuration failed: " + last_error_text());
	};
	auto configure = [&](int id) {
		try
		{
			send_config(id);
		}
		catch (const std::exception & e)
		{
			if (cb.log)
				cb.log(std::string("[vr-ipc] configuration failed: ") + e.what());
			disconnect(id);
		}
	};
	auto start_read = [&](peer & p) -> bool {
		ResetEvent(p.read_ov.hEvent);
		DWORD n = 0;
		const BOOL ok = ReadFile(p.pipe, p.buffer, sizeof(p.buffer), &n, &p.read_ov);
		const DWORD error = ok ? 0 : GetLastError();
		if (!ok && error != ERROR_IO_PENDING && error != ERROR_MORE_DATA)
			return false;
		p.reading = true;
		if (error != ERROR_IO_PENDING) // finished already: make sure the wait sees it (GetOverlappedResult has the result)
			SetEvent(p.read_ov.hEvent);
		return true;
	};
	auto start_listening = [&]() {
		if (!w->listening || w->listening == INVALID_HANDLE_VALUE)
			w->listening = CreateNamedPipeW(w->name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			                                PIPE_UNLIMITED_INSTANCES, pipe_buffer, pipe_buffer, 0, &w->security.attributes);
		if (w->listening == INVALID_HANDLE_VALUE)
		{
			w->listening = nullptr;
			return;
		}
		ResetEvent(w->connect_ov.hEvent);
		if (ConnectNamedPipe(w->listening, &w->connect_ov))
			SetEvent(w->connect_ov.hEvent);
		else if (GetLastError() == ERROR_PIPE_CONNECTED)
			SetEvent(w->connect_ov.hEvent);
		else if (GetLastError() == ERROR_IO_PENDING)
			w->connect_pending = true;
		else
		{
			CloseHandle(w->listening);
			w->listening = nullptr;
		}
	};
	auto accept = [&]() {
		w->connect_pending = false;
		HANDLE pipe = std::exchange(w->listening, nullptr);
		if (int(peers.size()) >= max_peers)
		{
			DisconnectNamedPipe(pipe);
			CloseHandle(pipe);
			return;
		}
		const int id = w->next_id++;
		peer & p = peers[id];
		p.pipe = pipe;
		p.read_ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		ULONG pid = 0;
		GetNamedPipeClientProcessId(pipe, &pid);
		p.pid = int(pid);
		p.process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid); // fails for elevated games: they pull the handles
		p.serial = ++w->next_serial;
		if (!start_read(p))
			disconnect(id);
	};
	auto handle_message = [&](int id, const uint8_t * buf, size_t n) {
		peer & p = peers.at(id);
		if (n < sizeof(ipc::msg))
		{
			disconnect(id);
			return;
		}
		ipc::msg type;
		std::memcpy(&type, buf, sizeof(type));
		if (!p.greeted)
		{
			ipc::hello_msg h;
			if (type != ipc::msg::hello || n < sizeof(h))
			{
				disconnect(id);
				return;
			}
			std::memcpy(&h, buf, sizeof(h));
			if (h.version != ipc::protocol_version)
			{
				if (cb.log)
					cb.log(std::format("[vr-ipc] refused a runtime of protocol {} (the streamer speaks {})", h.version, ipc::protocol_version));
				disconnect(id);
				return;
			}
			h.application[sizeof(h.application) - 1] = 0;
			p.greeted = true;
			p.app = std::format("{} (pid {})", h.application, p.pid);
			configure(id);
			if (!peers.contains(id))
				return;
			if (cb.log)
				cb.log("[vr-ipc] OpenXR app connected: " + p.app);
			if (client_fd.load() < 0)
				select(id);
			return;
		}
		if (type == ipc::msg::rendering || type == ipc::msg::submit)
		{
			if (client_fd.load() != id)
			{
				if (submitted.load())
				{
					if (cb.log)
						cb.log("[vr-ipc] a second rendering app was refused; probes may coexist");
					disconnect(id);
					return;
				}
				select(id);
			}
			if (!submitted.exchange(true) && cb.first_frame)
				cb.first_frame();
		}
		if (type == ipc::msg::submit && n >= sizeof(ipc::submit_msg))
		{
			if (stall_reported && cb.log)
				cb.log(std::format("[vr-ipc] frames again after {} ms",
				                   std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - last_submit).count()));
			stall_reported = false;
			last_submit = std::chrono::steady_clock::now();
			++submits;
			ipc::submit_msg s;
			std::memcpy(&s, buf, sizeof(s));
			bool queued = false;
			{
				std::lock_guard lock(mutex);
				const size_t per_slot = fovea_w > 0 && fovea_h > 0 ? 4 : 2;
				if (s.generation == generation && s.slot < p.pool.size() / per_slot && sink)
				{
					auto q = w->releases;
					std::vector<std::shared_ptr<const dmabuf_image>> images(p.pool.begin() + std::ptrdiff_t(per_slot * s.slot),
					                                                        p.pool.begin() + std::ptrdiff_t(per_slot * (s.slot + 1)));
					sink->submit(std::move(images), s.info, [this, q, id, serial = p.serial, g = s.generation, sl = s.slot] {
						{
							std::lock_guard l(q->m);
							if (!q->alive)
								return;
							q->items.push_back({id, serial, g, sl});
						}
						wake('r');
					});
					queued = true;
				}
			}
			if (!queued)
			{
				ipc::release_msg r;
				r.generation = s.generation;
				r.slot = s.slot;
				write_message(p.pipe, &r, sizeof(r));
			}
		}
		else if (type == ipc::msg::haptics && id == client_fd.load() && n >= sizeof(ipc::haptics_msg) && cb.haptics)
		{
			ipc::haptics_msg h;
			std::memcpy(&h, buf, sizeof(h));
			cb.haptics(h);
		}
		else if (type == ipc::msg::recenter && id == client_fd.load() && cb.recenter)
			cb.recenter();
		else if (type == ipc::msg::render_thread && n >= sizeof(ipc::render_thread_msg) && cb.render_thread)
		{
			ipc::render_thread_msg m;
			std::memcpy(&m, buf, sizeof(m));
			if (m.tid > 0)
				cb.render_thread(p.pid, m.tid); // Windows has no PID namespaces: the id is the system's
		}
		else if (type == ipc::msg::log && n >= sizeof(ipc::log_msg) && cb.log)
		{
			ipc::log_msg m;
			std::memcpy(&m, buf, sizeof(m));
			m.text[sizeof(m.text) - 1] = 0;
			std::string text(m.text);
			while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
				text.pop_back();
			if (p.log_lines < 300)
				cb.log(std::format("[app {}] {}", p.pid, text));
			else if (p.log_lines == 300)
				cb.log(std::format("[app {}] (further runtime messages dropped)", p.pid));
			++p.log_lines;
		}
	};

	start_listening();
	while (!quit)
	{
		std::vector<HANDLE> waits{w->wake_event};
		std::vector<int> ids{0};
		if (w->listening)
		{
			waits.push_back(w->connect_ov.hEvent);
			ids.push_back(-1);
		}
		for (auto & [id, p]: peers)
			if (p.reading)
			{
				waits.push_back(p.read_ov.hEvent);
				ids.push_back(id);
			}
		if (submitted.load() && client_fd.load() >= 0 && last_submit.time_since_epoch().count())
		{
			auto idle = std::chrono::steady_clock::now() - last_submit;
			if (!stall_reported && idle > std::chrono::seconds(5) && state->vr_active)
			{
				stall_reported = true;
				if (cb.log)
					cb.log(std::format("[vr-ipc] no frames from the app for {} s (last submit after {} frames)",
					                   std::chrono::duration_cast<std::chrono::seconds>(idle).count(), submits));
			}
		}
		// clients that connect but never say hello
		for (auto it = peers.begin(); it != peers.end();)
		{
			const int id = it->first;
			const bool stale = !it->second.greeted && std::chrono::steady_clock::now() - it->second.connected > std::chrono::seconds(2);
			++it;
			if (stale)
				disconnect(id);
		}
		DWORD r = WaitForMultipleObjects(DWORD(waits.size()), waits.data(), FALSE, 1000);
		if (r == WAIT_TIMEOUT)
			continue;
		if (r == WAIT_FAILED)
			break;
		const size_t index = r - WAIT_OBJECT_0;
		if (index >= waits.size())
			continue;
		if (ids[index] == 0)
		{
			std::string reasons;
			{
				std::lock_guard lock(w->wake_mutex);
				reasons.swap(w->wake_reasons);
			}
			if (quit)
				break;
			std::vector<win32::release_queue::item> done;
			{
				std::lock_guard lock(w->releases->m);
				done.swap(w->releases->items);
			}
			for (const auto & d: done)
				if (auto it = peers.find(d.id); it != peers.end() && it->second.serial == d.serial)
				{
					ipc::release_msg rm;
					rm.generation = d.generation;
					rm.slot = d.slot;
					write_message(it->second.pipe, &rm, sizeof(rm));
				}
			if (config_dirty.exchange(false))
			{
				std::vector<int> greeted;
				for (const auto & [id, p]: peers)
					if (p.greeted)
						greeted.push_back(id);
				for (int id: greeted)
					configure(id);
			}
			continue;
		}
		if (ids[index] == -1)
		{
			DWORD unused;
			if (GetOverlappedResult(w->listening, &w->connect_ov, &unused, FALSE) || GetLastError() == ERROR_PIPE_CONNECTED)
				accept();
			else
			{
				CloseHandle(w->listening);
				w->listening = nullptr;
				w->connect_pending = false;
			}
			start_listening();
			continue;
		}
		const int id = ids[index];
		auto it = peers.find(id);
		if (it == peers.end())
			continue;
		peer & p = it->second;
		DWORD n = 0;
		const BOOL ok = GetOverlappedResult(p.pipe, &p.read_ov, &n, FALSE);
		const DWORD error = ok ? 0 : GetLastError();
		p.reading = false;
		if (!ok && error == ERROR_IO_INCOMPLETE)
		{
			p.reading = true;
			ResetEvent(p.read_ov.hEvent);
			continue;
		}
		if (!ok && error == ERROR_MORE_DATA)
		{
			// longer than any message of the protocol: drop the connection rather than parse a fragment
			disconnect(id);
			continue;
		}
		if (!ok)
		{
			disconnect(id); // ERROR_BROKEN_PIPE: the app went away
			continue;
		}
		handle_message(id, p.buffer, n);
		if (auto again = peers.find(id); again != peers.end() && !start_read(again->second))
			disconnect(id);
	}
	std::vector<int> all;
	for (const auto & [id, p]: peers)
		all.push_back(id);
	for (int id: all)
	{
		std::lock_guard lock(mutex);
		w->close_peer(peers.at(id));
		peers.erase(id);
	}
	client_fd = -1;
	peer_pid = 0;
	submitted = false;
}
} // namespace od
#endif
