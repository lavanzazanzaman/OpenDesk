// Streamer <-> OpenXR runtime (libovr-opendesk, inside the game process) interface. Plain C++ structs, same machine.
//
//   socket  : $XDG_RUNTIME_DIR/opendesk/vr.sock, SOCK_SEQPACKET (message boundaries + fd passing)
//             client -> hello; server -> config + fds [tracking memfd, pool images L0,R0,L1,R1,...];
//             client renders into a slot's two images (defoveation-warped, encoded size), then -> submit(slot);
//             server encodes + sends them to the headset, then -> release(slot). Config is resent when it changes
//             (the client drops its old imports). haptics / recenter go client -> server.
//   tracking: the memfd holds the headset's latest FrameData verbatim (head, controllers, hands, eyes, face, body;
//             offsets in frame_data.h) plus display parameters, under a seqlock (read with read_shared_state).
#pragma once

#include "vr_types.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace od::ipc
{
constexpr uint32_t protocol_version = 15;
constexpr size_t frame_data_size = 4992; // Upstream.Mobile.FrameData
constexpr size_t skeleton_joint_bytes = 36, skeleton_max_joints = 84; // SkeletonDataChanged: joint, parent, pose
constexpr uint32_t max_slots = 4;

std::string socket_path(); // $XDG_RUNTIME_DIR/opendesk/vr.sock
// the same service on an abstract socket ("\0opendesk-<uid>/vr"): abstract names live in the network namespace, not
// the file system, so apps inside Steam's pressure-vessel / flatpak containers (no $XDG_RUNTIME_DIR/opendesk) still
// reach it. No file permissions apply there: both ends check the peer's uid (same_user).
std::string abstract_name();
bool same_user(int fd); // SO_PEERCRED uid == ours
bool service_available(); // either socket is listening (no connection made)

// the clock of display_time_ns / frame_data_time_ns / next_frame_ns: CLOCK_MONOTONIC on Linux, QueryPerformanceCounter
// on Windows (both ends of the connection read the same one)
int64_t monotonic_ns();

enum class msg : uint32_t
{
	hello = 1,   // client -> server
	config = 2,  // server -> client (+ fds)
	submit = 3,  // client -> server
	release = 4, // server -> client
	haptics = 5, // client -> server
	recenter = 6, // client -> server
	rendering = 7, // client -> server: the app submits frames (sent once, also while the headset isn't in VR yet)
	log = 8,       // client -> server: a runtime message for the streamer's log
	render_thread = 9 // client -> server: the thread that submits frames ("Boost game priority")
};

struct hello_msg
{
	msg type = msg::hello;
	uint32_t version = protocol_version;
	int32_t pid = 0;
	char application[64] = {};
};

constexpr uint32_t memory_dmabuf = 0;        // DMA-BUF image: fourcc + DRM modifier, plane at offset/stride
constexpr uint32_t memory_opaque_buffer = 1; // Vulkan OPAQUE_FD memory of `size` bytes, only on the GPU with
                                             // device_uuid/driver_uuid: NV12 or P010 (fourcc), rows of `stride`
                                             // bytes, chroma at uv_offset, BT.709 (full_range or limited)
// Every VkBuffer bound to that memory (exporter and importers) must be created identically - same size
// (image_desc.buffer_bytes, not the allocation size) and these usage flags - or drivers may map it differently
// (NVIDIA: GPU fault, VK_ERROR_DEVICE_LOST). Values: TRANSFER_SRC | TRANSFER_DST | STORAGE_BUFFER.
constexpr uint32_t opaque_buffer_usage = 0x1 | 0x2 | 0x20;
constexpr uint32_t fourcc_nv12 = 0x3231564e; // DRM_FORMAT_NV12
constexpr uint32_t fourcc_p010 = 0x30313050; // DRM_FORMAT_P010

struct image_desc
{
	uint32_t width, height, stride, offset, fourcc;
	uint64_t modifier;
	uint32_t memory = memory_dmabuf;
	uint64_t size = 0;
	uint32_t uv_offset = 0, full_range = 0;
	uint64_t buffer_bytes = 0; // opaque buffers: the VkBuffer size (size: the allocation's)
	uint32_t host_visible = 0; // opaque buffers in system memory (another GPU encodes; the exporter's memory type must be matched)
	uint8_t device_uuid[16] = {}, driver_uuid[16] = {};
};

struct config_msg
{
	msg type = msg::config;
	uint32_t version = protocol_version;
	uint32_t generation = 0;      // bumps on every reconfiguration
	uint32_t eye_width = 0, eye_height = 0; // recommended render size per eye (the app's swapchains)
	uint32_t fps = 72;
	uint32_t slot_count = 0;      // image sets that follow as fds (after the tracking memfd): per slot left, right
	                              // (+ left fovea, right fovea when foveated)
	image_desc image{};           // the eye images (encoded size: periphery when foveated)
	uint32_t foveated = 0;        // foveated streaming: also a fovea image per eye (unwarped crop at render density)
	image_desc fovea_image{};
	uint32_t shared_state_size = 0;
	uint32_t render_major = 0, render_minor = 0; // the VR GPU's render node
	uint32_t gpu_vendor = 0, gpu_device = 0;
};

struct render_settings
{
	float horizontal_fov = 1.f, vertical_fov = 1.f, resolution_scale = 1.f;
	float gamma = 1.f, sharpening = 0.f;
	uint32_t use_stencil = 1;
	// foveated streaming: the fovea crop loses detail toward its edge and ends in the periphery's own pixels (off: a
	// hard edge like the Windows streamer, softened only by the headset's thin blend ring)
	uint32_t soft_fovea_edge = 0;
	uint32_t features = 0, emulate_trackers = 0, emulate_index = 0;
	float boundary_width = 0, boundary_height = 0;
	uint32_t recenter_serial = 0;
	// Synchronous Spacewarp (the headset's SharedMobileSettings.UseSpacewarp, Automatic or Always): the app renders every
	// other headset frame and the headset synthesizes the ones in between (Quest: its App SpaceWarp), like Windows'
	// upstream runtime, which halves the app's frame rate and reports ASW active
	uint32_t spacewarp = 0;
};

struct submit_msg
{
	msg type = msg::submit;
	uint32_t generation = 0;
	uint32_t slot = 0;
	vr_render_info info{};        // poses the frame was rendered with (RenderPose for reprojection)
	int64_t display_time_ns = 0;  // CLOCK_MONOTONIC, predicted display time the app rendered for
};

struct release_msg
{
	msg type = msg::release;
	uint32_t generation = 0;
	uint32_t slot = 0;
};

struct haptics_msg
{
	msg type = msg::haptics;
	uint32_t hand = 0;         // 0 left, 1 right
	float amplitude = 0;       // 0..1
	float frequency = 0;       // Hz, 0 = default
	uint32_t duration_ms = 0;  // 0 = until changed
};

struct recenter_msg
{
	msg type = msg::recenter;
};

struct log_msg
{
	msg type = msg::log;
	char text[500] = {}; // NUL-terminated
};

struct rendering_msg
{
	msg type = msg::rendering;
};

struct render_thread_msg
{
	msg type = msg::render_thread;
	int32_t tid = 0; // gettid() in the app's PID namespace (Steam's container may have its own)
};

struct shared_state
{
	std::atomic<uint32_t> seq;       // odd while the streamer writes
	uint32_t version;
	uint32_t headset_connected;      // a headset session exists
	uint32_t vr_active;              // the headset is in VR mode (StreamingSource VR): frames are wanted
	int64_t frame_data_time_ns;      // CLOCK_MONOTONIC arrival of frame_data
	int64_t next_frame_ns;           // pacing hint: when the streamer wants the next frame submitted
	int64_t frame_period_ns;
	float ipd;
	fov_port fov[2];
	float floor_height;
	uint32_t hmd_type;
	render_settings settings;
	uint32_t gaze_valid;             // foveated streaming: the headset's latest eye positions
	float gaze[2][2];                // per eye x, y in [-0.5, 0.5] of the eye's FOV (+y down); fovea centre = (g/2+0.5)*size
	uint8_t frame_data[frame_data_size];
	// body tracking skeleton (SkeletonDataChanged): joint count (70 / 84, 0: none yet), the headset's change counter
	uint32_t skeleton_joint_count;
	int32_t skeleton_changed_count;
	uint8_t skeleton[skeleton_joint_bytes * skeleton_max_joints];
};

// consistent copy of the shared state (seqlock reader)
inline void read_shared_state(const shared_state * s, shared_state & out)
{
	for (;;)
	{
		uint32_t a = s->seq.load(std::memory_order_acquire);
		if (a & 1)
			continue;
		std::memcpy(reinterpret_cast<uint8_t *>(&out) + sizeof(out.seq), reinterpret_cast<const uint8_t *>(s) + sizeof(s->seq),
		            sizeof(shared_state) - sizeof(s->seq));
		std::atomic_thread_fence(std::memory_order_acquire);
		if (s->seq.load(std::memory_order_relaxed) == a)
			return;
	}
}
} // namespace od::ipc
