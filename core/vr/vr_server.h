// Streamer side of vr_ipc.h: accepts runtime probes alongside one rendering client; shares tracking and hands each a
// pool of DMA-BUF image pairs to render into. Submitted frames go to the frame sink (the VR encoder) on the server
// thread; the slot is released right after it was encoded.
#pragma once

#include "dmabuf.h"
#include "vk_buffers.h"
#include "vr_ipc.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace od
{
class vr_server
{
public:
	void set_settings(const ipc::render_settings &);
	// foveated streaming: a fovea image of this size per eye too (0 x 0: off); reconfigures the app when it changes
	void set_foveation(int fovea_width, int fovea_height);
	void set_gaze(bool valid, const float gaze[2][2]); // the headset's eye positions (see ipc::shared_state::gaze)
	void set_skeleton(const uint8_t * payload, size_t size); // SkeletonDataChanged as received
	struct frame_sink
	{
		virtual ~frame_sink() = default;
		// the sink keeps the images while it needs them and calls done() exactly once when it is finished with them
		// (any thread); only then the app gets the slot back
		// images: left, right (+ left fovea, right fovea when foveated)
		virtual void submit(std::vector<std::shared_ptr<const dmabuf_image>> images, const vr_render_info &, std::function<void()> done) = 0;
	};
	struct callbacks
	{
		std::function<void(std::string)> log;                       // server thread
		std::function<void(bool connected, std::string app)> client; // server thread
		std::function<void()> first_frame;                          // server thread: the connected app renders
		std::function<void(const ipc::haptics_msg &)> haptics;      // server thread
		std::function<void()> recenter;                             // server thread
		std::function<void(int pid, int tid)> render_thread;        // server thread: the app's frame-submitting thread (host ids)
	};

	vr_server(const std::string & render_node, callbacks);
	~vr_server();

	// engine thread
	void update_tracking(const uint8_t * frame_data, size_t size);
	void set_display(bool headset_connected, bool vr_active, float ipd, const fov_port fov[2], int hmd_type);
	// stream geometry; reallocates the pool (and reconfigures the client) when it changes
	// ten_bit / full_range: the YUV layout of Vulkan stream buffers (NVIDIA), matching the encoder
	void set_stream(int eye_width, int eye_height, int encoded_width, int encoded_height, int fps, bool ten_bit = false,
	                bool full_range = false);
	void set_sink(std::shared_ptr<frame_sink>); // nullptr: submitted frames are released unencoded
	bool has_client() const { return client_fd.load() >= 0; }
	// the connected app renders (announced it or submitted a frame). Probing clients - Proton's launcher shim, WineOpenXR's
	// test instance, RiftLift's headless sessions - connect and leave without ever submitting.
	bool rendering() const { return has_client() && submitted.load(); }
	int client_pid() const { return has_client() ? peer_pid.load() : 0; } // in our pid namespace (SO_PEERCRED; Windows: the pipe client)

private:
	std::string render_node;
	void run();
	void write_begin();
	void write_end();

	callbacks cb;
	std::unique_ptr<vk_buffer_pool> vk_pool; // NVIDIA (Windows: every GPU): exportable Vulkan memory (vk_buffers.h)
	ipc::shared_state * state = nullptr;
	std::atomic<int> client_fd{-1}; // the selected client (Windows: its connection id)
	std::atomic<int> peer_pid{0};
	std::atomic<bool> submitted{false};
	std::atomic<bool> quit{false};
	// server thread: stall detection
	std::chrono::steady_clock::time_point last_submit{};
	uint64_t submits = 0;
	bool stall_reported = false;

	std::mutex mutex; // pool, geometry, sink
	using image_ptr = std::shared_ptr<dmabuf_image>; // the encoder may still hold a frame's images after a reconfiguration
	uint32_t generation = 0;
	int eye_w = 0, eye_h = 0, enc_w = 0, enc_h = 0, fps = 72;
	bool ten_bit = false, full_range = false;
	int fovea_w = 0, fovea_h = 0;
	std::atomic<bool> config_dirty{false};
	std::shared_ptr<frame_sink> sink;
	void wake(char reason); // 'c' configuration, 'q' quit
#ifdef _WIN32
	struct win32; // vr_server_win.cpp: pipe instances, peers, the tracking section
	std::unique_ptr<win32> w;
#else
	void send_config(int fd); // server thread (or under mutex)
	gbm_allocator alloc;
	int listen_fd = -1;
	int abstract_fd = -1; // same service for containerized apps (ipc::abstract_name)
	int memfd = -1;
	int wake_pipe[2] = {-1, -1};
	struct peer { std::string app; int pid = 0; std::vector<image_ptr> pool; int log_lines = 0; uint64_t serial = 0; };
	uint64_t next_peer_serial = 0;
	// slots the sink finished with, released to their app from the server thread (wake_pipe 'r')
	struct release_queue
	{
		struct item { int fd; uint64_t serial; uint32_t generation, slot; };
		std::mutex m;
		std::vector<item> items;
		int wake = -1;
		bool alive = true;
	};
	std::shared_ptr<release_queue> releases = std::make_shared<release_queue>();
	std::map<int, peer> peers; // server thread only; separate buffers prevent cross-client writes
#endif
	std::thread thread;
};
} // namespace od
