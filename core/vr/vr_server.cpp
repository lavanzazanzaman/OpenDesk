#ifndef _WIN32 // the Windows transport is in vr_server_win.cpp
#include "vr_server.h"
#include "hardware_encoder.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <ctime>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <filesystem>
#include <sys/sysmacros.h>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace od
{
namespace
{
bool send_with_fds(int fd, const void * data, size_t size, const std::vector<int> & fds)
{
	iovec iov{const_cast<void *>(data), size};
	msghdr m{};
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	std::vector<char> control(CMSG_SPACE(sizeof(int) * fds.size()));
	if (!fds.empty())
	{
		m.msg_control = control.data();
		m.msg_controllen = control.size();
		cmsghdr * c = CMSG_FIRSTHDR(&m);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
		std::memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
	}
	return sendmsg(fd, &m, MSG_NOSIGNAL) == ssize_t(size);
}
} // namespace

vr_server::vr_server(const std::string & render_node, callbacks c) :
        render_node(render_node), cb(std::move(c)), alloc(render_node)
{
	// NVIDIA: exportable Vulkan memory (NVENC imports it through CUDA; its GBM has no linear render buffers).
	// OPENDESK_VR_BUFFERS=opaque|dmabuf picks either on any GPU (tests).
	const char * want = std::getenv("OPENDESK_VR_BUFFERS");
	if (want ? std::string(want) == "opaque" : gpu_vendor(render_node) == 0x10de || vr_encode_node() != render_node) // (another GPU encodes: read back through this one)
		try
		{
			vk_pool = std::make_unique<vk_buffer_pool>(render_node, vr_encode_node() != render_node);
		}
		catch (const std::exception & e)
		{
			if (cb.log)
				cb.log(std::string("[vr-ipc] Vulkan stream buffers unavailable, using GBM: ") + e.what());
		}
	std::string path = ipc::socket_path();
	if (path.size() >= sizeof(sockaddr_un{}.sun_path))
		throw std::runtime_error("socket path too long: " + path);
	std::string dir = path.substr(0, path.rfind('/'));
	mkdir(dir.c_str(), 0700);
	chmod(dir.c_str(), 0700); // only this user may connect
	unlink(path.c_str());
	listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
	if (listen_fd < 0 || bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || listen(listen_fd, 4) < 0)
		throw std::runtime_error("cannot listen on " + path + ": " + strerror(errno));
	std::string name = ipc::abstract_name();
	abstract_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	sockaddr_un abstract{};
	abstract.sun_family = AF_UNIX;
	std::memcpy(abstract.sun_path, name.data(), name.size());
	if (abstract_fd < 0 ||
	    bind(abstract_fd, reinterpret_cast<sockaddr *>(&abstract), socklen_t(offsetof(sockaddr_un, sun_path) + name.size())) < 0 ||
	    listen(abstract_fd, 4) < 0)
	{
		// taken (a second streamer, or someone squatting it): apps outside containers still use the path socket
		if (cb.log)
			cb.log(std::format("[vr-ipc] abstract socket unavailable ({}): apps in Steam's container won't find VR", strerror(errno)));
		if (abstract_fd >= 0)
			close(abstract_fd);
		abstract_fd = -1;
	}

	memfd = memfd_create("vd-vr-tracking", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (memfd < 0 || ftruncate(memfd, sizeof(ipc::shared_state)) < 0)
		throw std::runtime_error("memfd_create failed");
	fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
	void * p = mmap(nullptr, sizeof(ipc::shared_state), PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
	if (p == MAP_FAILED)
		throw std::runtime_error("mmap failed");
	state = new (p) ipc::shared_state{};
	state->version = ipc::protocol_version;
	state->ipd = 0.063f;
	state->frame_period_ns = 1000000000 / 72;

	if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
		throw std::runtime_error("pipe2 failed");
	releases->wake = wake_pipe[1];
	// default geometry (Quest 3, Low) until a headset tells us better: apps can start before the headset connects
	set_stream(1728, 1824, 1152, 1216, 72);
	thread = std::thread(&vr_server::run, this);
}

vr_server::~vr_server()
{
	{
		std::lock_guard lock(releases->m); // late done() calls from an encoder become no-ops
		releases->alive = false;
	}
	quit = true;
	[[maybe_unused]] ssize_t r = write(wake_pipe[1], "q", 1);
	if (thread.joinable())
		thread.join();
	if (int fd = client_fd.exchange(-1); fd >= 0)
		close(fd);
	close(listen_fd);
	if (abstract_fd >= 0)
		close(abstract_fd);
	unlink(ipc::socket_path().c_str());
	close(wake_pipe[0]);
	close(wake_pipe[1]);
	if (state)
		munmap(state, sizeof(ipc::shared_state));
	close(memfd);
}

void vr_server::wake(char reason)
{
	[[maybe_unused]] ssize_t r = write(wake_pipe[1], &reason, 1);
}

void vr_server::send_config(int fd)
{
	if (fd < 0)
		return;
	std::lock_guard lock(mutex);
	auto & pool = peers.at(fd).pool;
	pool.clear();
	const bool foveated = fovea_w > 0 && fovea_h > 0;
	const uint32_t per_slot = foveated ? 4 : 2;
	auto allocate = [&](int w, int h) -> image_ptr {
		return vk_pool ? image_ptr(vk_pool->allocate(uint32_t(w), uint32_t(h), ten_bit)) : image_ptr(alloc.allocate(uint32_t(w), uint32_t(h)));
	};
	for (uint32_t slot = 0; slot < 3; ++slot) // per slot: left, right (+ left fovea, right fovea)
		for (uint32_t k = 0; k < per_slot; ++k)
			pool.push_back(k < 2 ? allocate(enc_w, enc_h) : allocate(fovea_w, fovea_h));
	ipc::config_msg c;
	c.generation = generation;
	c.eye_width = uint32_t(eye_w);
	c.eye_height = uint32_t(eye_h);
	c.fps = uint32_t(fps);
	c.slot_count = uint32_t(pool.size() / per_slot);
	const dmabuf_image & i = *pool[0];
	c.image = {i.width, i.height, i.stride, i.offset, i.fourcc, i.modifier};
	c.image.memory = i.memory;
	c.image.size = i.size;
	c.image.uv_offset = i.uv_offset;
	c.image.buffer_bytes = i.buffer_bytes;
	c.image.host_visible = i.host_visible;
	c.image.full_range = full_range;
	if (vk_pool)
	{
		std::memcpy(c.image.device_uuid, vk_pool->device_uuid().data(), 16);
		std::memcpy(c.image.driver_uuid, vk_pool->driver_uuid().data(), 16);
	}
	if (foveated)
	{
		const dmabuf_image & f = *pool[2];
		c.foveated = 1;
		c.fovea_image = c.image; // same memory kind, format, range and GPU
		c.fovea_image.width = f.width;
		c.fovea_image.height = f.height;
		c.fovea_image.stride = f.stride;
		c.fovea_image.offset = f.offset;
		c.fovea_image.modifier = f.modifier;
		c.fovea_image.size = f.size;
		c.fovea_image.uv_offset = f.uv_offset;
		c.fovea_image.buffer_bytes = f.buffer_bytes;
		c.fovea_image.host_visible = f.host_visible;
		if (cb.log)
			cb.log(std::format("[vr-ipc] foveated: {}x{} fovea per eye", f.width, f.height));
	}
	if (cb.log)
		cb.log(std::format("[vr-ipc] {} buffers {}x{} on {}, {}", pool.size(), i.width, i.height, render_node,
		                   i.memory == ipc::memory_opaque_buffer ? std::format("Vulkan memory (OPAQUE_FD), {} {} range",
		                                                                       ten_bit ? "P010" : "NV12", full_range ? "full" : "limited")
		                   : i.modifier                          ? std::format("tiled (modifier {:#x})", i.modifier)
		                                                         : std::string("linear")));
	c.shared_state_size = sizeof(ipc::shared_state);
	struct stat node{};
	if (stat(render_node.c_str(), &node) == 0) {
		c.render_major = major(node.st_rdev); c.render_minor = minor(node.st_rdev);
	}
	auto sys = std::filesystem::path("/sys/class/drm") / std::filesystem::path(render_node).filename() / "device";
	std::ifstream(sys / "vendor") >> std::hex >> c.gpu_vendor;
	std::ifstream(sys / "device") >> std::hex >> c.gpu_device;
	std::vector<int> fds{memfd};
	for (const auto & img: pool)
		fds.push_back(img->fd);
	if (!send_with_fds(fd, &c, sizeof(c), fds) && cb.log)
		cb.log(std::format("[vr-ipc] sending the configuration failed: {}", strerror(errno)));
}

void vr_server::run()
{
    auto select = [&](int fd) {
        client_fd = fd;
        submitted = false;
        peer_pid = fd >= 0 ? peers.at(fd).pid : 0;
        if (fd >= 0 && cb.client) cb.client(true, peers.at(fd).app);
    };
    auto disconnect = [&](int fd) {
        auto it = peers.find(fd);
        if (it == peers.end()) return;
        const bool selected = client_fd.load() == fd;
        const auto app = it->second.app;
        close(fd);
        peers.erase(it);
        if (cb.log) cb.log("[vr-ipc] OpenXR app disconnected: " + app);
        if (selected) {
            client_fd = -1;
            submitted = false;
            peer_pid = 0;
            if (cb.client) cb.client(false, app);
            if (!peers.empty()) select(peers.begin()->first);
        }
    };
    auto configure = [&](int fd) {
        try { send_config(fd); }
        catch (const std::exception & e) {
            if (cb.log) cb.log(std::string("[vr-ipc] configuration failed: ") + e.what());
            disconnect(fd);
        }
    };
    while (!quit)
    {
        std::vector<pollfd> p{{wake_pipe[0], POLLIN, 0}, {listen_fd, POLLIN, 0}, {abstract_fd, POLLIN, 0}};
        for (const auto & [fd, peer]: peers) p.push_back({fd, POLLIN, 0});
        // a rendering app that stops submitting (hung, loading, crashed GPU work): say so once, and when it resumes
        if (submitted.load() && client_fd.load() >= 0 && last_submit.time_since_epoch().count()) {
            auto idle = std::chrono::steady_clock::now() - last_submit;
            if (!stall_reported && idle > std::chrono::seconds(5) && state->vr_active) {
                stall_reported = true;
                if (cb.log) cb.log(std::format("[vr-ipc] no frames from the app for {} s (last submit after {} frames)",
                                               std::chrono::duration_cast<std::chrono::seconds>(idle).count(), submits));
            }
        }
        if (poll(p.data(), p.size(), 1000) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (p[0].revents & POLLIN) {
            char buf[64];
            while (read(wake_pipe[0], buf, sizeof(buf)) > 0) {}
            if (quit) break;
            // slots the encoder is done with -> their app (if it is still the same connection)
            std::vector<release_queue::item> done;
            {
                std::lock_guard lock(releases->m);
                done.swap(releases->items);
            }
            for (const auto & d: done)
                if (auto it = peers.find(d.fd); it != peers.end() && it->second.serial == d.serial) {
                    ipc::release_msg r; r.generation = d.generation; r.slot = d.slot;
                    send(d.fd, &r, sizeof(r), MSG_NOSIGNAL);
                }
            if (config_dirty.exchange(false)) {
                std::vector<int> fds;
                for (const auto & [fd, peer]: peers) fds.push_back(fd);
                for (int fd: fds) configure(fd);
            }
        }
        for (int l: {1, 2}) {
            if (!(p[l].revents & POLLIN)) continue;
            int fd = accept4(p[l].fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) continue;
            if (!ipc::same_user(fd) || peers.size() >= 8) { close(fd); continue; }
            ipc::hello_msg h;
            pollfd hp{fd, POLLIN, 0};
            if (poll(&hp, 1, 2000) <= 0 || recv(fd, &h, sizeof(h), 0) != ssize_t(sizeof(h)) ||
                h.type != ipc::msg::hello || h.version != ipc::protocol_version) {
                close(fd); continue;
            }
            h.application[sizeof(h.application)-1] = 0;
            ucred cred{}; socklen_t size = sizeof(cred);
            getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size);
            peer & added = peers[fd];
            added.serial = ++next_peer_serial;
            added.pid = cred.pid;
            added.app = std::format("{} (pid {})", h.application, cred.pid);
            configure(fd);
            if (!peers.contains(fd)) continue;
            if (cb.log) cb.log("[vr-ipc] OpenXR app connected: " + peers.at(fd).app);
            if (client_fd.load() < 0) select(fd);
        }
        for (size_t i = 3; i < p.size(); ++i) {
            int fd = p[i].fd;
            if (!peers.contains(fd) || !(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            alignas(8) uint8_t buf[sizeof(ipc::log_msg) + 8];
            ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n < ssize_t(sizeof(ipc::msg))) { disconnect(fd); continue; }
            ipc::msg type; std::memcpy(&type, buf, sizeof(type));
            if (type == ipc::msg::rendering || type == ipc::msg::submit) {
                if (client_fd.load() != fd) {
                    if (submitted.load()) {
                        if (cb.log) cb.log("[vr-ipc] a second rendering app was refused; probes may coexist");
                        disconnect(fd); continue;
                    }
                    select(fd);
                }
                if (!submitted.exchange(true) && cb.first_frame) cb.first_frame();
            }
            if (type == ipc::msg::submit && size_t(n) >= sizeof(ipc::submit_msg)) {
                if (stall_reported && cb.log)
                    cb.log(std::format("[vr-ipc] frames again after {} ms",
                                       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - last_submit).count()));
                stall_reported = false;
                last_submit = std::chrono::steady_clock::now();
                ++submits;
                ipc::submit_msg s; std::memcpy(&s, buf, sizeof(s));
                bool queued = false;
                {
                    std::lock_guard lock(mutex);
                    auto & pool = peers.at(fd).pool;
                    const size_t per_slot = fovea_w > 0 && fovea_h > 0 ? 4 : 2;
                    if (s.generation == generation && s.slot < pool.size()/per_slot && sink) {
                        // encoded on the sink's thread; the slot goes back to the app once the encoder is done with it
                        auto q = releases;
                        uint64_t serial = peers.at(fd).serial;
                        std::vector<std::shared_ptr<const dmabuf_image>> images(pool.begin() + std::ptrdiff_t(per_slot * s.slot),
                                                                                pool.begin() + std::ptrdiff_t(per_slot * (s.slot + 1)));
                        sink->submit(std::move(images), s.info, [q, fd, serial, g = s.generation, sl = s.slot] {
                            std::lock_guard l(q->m);
                            if (!q->alive) return;
                            q->items.push_back({fd, serial, g, sl});
                            [[maybe_unused]] ssize_t w = write(q->wake, "r", 1);
                        });
                        queued = true;
                    }
                }
                if (!queued) {
                    ipc::release_msg r; r.generation = s.generation; r.slot = s.slot;
                    send(fd, &r, sizeof(r), MSG_NOSIGNAL);
                }
            } else if (type == ipc::msg::haptics && fd == client_fd.load() &&
                       size_t(n) >= sizeof(ipc::haptics_msg) && cb.haptics) {
                ipc::haptics_msg h; std::memcpy(&h, buf, sizeof(h)); cb.haptics(h);
            } else if (type == ipc::msg::recenter && fd == client_fd.load() && cb.recenter) cb.recenter();
            else if (type == ipc::msg::render_thread && size_t(n) >= sizeof(ipc::render_thread_msg) && cb.render_thread) {
                // the app's thread id may be from its own PID namespace (Steam's container): find the task whose
                // innermost NSpid is that id
                ipc::render_thread_msg m; std::memcpy(&m, buf, sizeof(m));
                const int pid = peers.at(fd).pid;
                int host_tid = 0;
                std::error_code ec;
                if (std::filesystem::exists(std::format("/proc/{}/task/{}", pid, m.tid), ec))
                    host_tid = m.tid;
                else
                    for (const auto & t: std::filesystem::directory_iterator(std::format("/proc/{}/task", pid), ec)) {
                        std::ifstream st(t.path() / "status");
                        for (std::string line; std::getline(st, line);)
                            if (line.starts_with("NSpid:")) {
                                std::istringstream ids(line.substr(6));
                                int first = 0, last = 0, v;
                                for (bool head = true; ids >> v; head = false) { if (head) first = v; last = v; }
                                if (last == m.tid) host_tid = first;
                            }
                        if (host_tid) break;
                    }
                if (host_tid > 0) cb.render_thread(pid, host_tid);
            }
            else if (type == ipc::msg::log && size_t(n) >= sizeof(ipc::log_msg) && cb.log) {
                // runtime messages from inside the app (libovr-opendesk, OpenDesk XR), bounded per app
                auto & peer = peers.at(fd);
                ipc::log_msg m; std::memcpy(&m, buf, sizeof(m));
                m.text[sizeof(m.text) - 1] = 0;
                std::string text(m.text);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                if (peer.log_lines < 300)
                    cb.log(std::format("[app {}] {}", peer.pid, text));
                else if (peer.log_lines == 300)
                    cb.log(std::format("[app {}] (further runtime messages dropped)", peer.pid));
                ++peer.log_lines;
            }
        }
    }
    for (const auto & [fd, peer]: peers) close(fd);
    peers.clear();
    client_fd = -1;
    peer_pid = 0;
    submitted = false;
}
} // namespace od
#endif
