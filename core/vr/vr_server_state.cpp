// The platform-neutral half of vr_server: the shared tracking state (seqlock writer), stream geometry and settings.
// The transport (Unix sockets + fds on Linux, a named pipe + NT handles on Windows) is in vr_server.cpp / vr_server_win.cpp.
#include "vr_server.h"

#include <algorithm>
#include <cstring>

namespace od
{
void vr_server::write_begin()
{
	state->seq.fetch_add(1, std::memory_order_acq_rel); // odd: writing
	std::atomic_thread_fence(std::memory_order_release);
}

void vr_server::write_end()
{
	std::atomic_thread_fence(std::memory_order_release);
	state->seq.fetch_add(1, std::memory_order_release);
}

void vr_server::update_tracking(const uint8_t * fd, size_t size)
{
	write_begin();
	std::memcpy(state->frame_data, fd, std::min(size, ipc::frame_data_size));
	state->frame_data_time_ns = ipc::monotonic_ns();
	if (size >= 12)
		std::memcpy(&state->floor_height, fd + 8, 4);
	write_end();
}

void vr_server::set_display(bool connected, bool vr_active, float ipd, const fov_port fov[2], int hmd_type)
{
	write_begin();
	state->headset_connected = connected;
	state->vr_active = vr_active;
	state->ipd = ipd;
	state->fov[0] = fov[0];
	state->fov[1] = fov[1];
	state->hmd_type = uint32_t(hmd_type);
	write_end();
}

void vr_server::set_stream(int ew, int eh, int cw, int ch, int f, bool ten, bool full)
{
	{
		std::lock_guard lock(mutex);
		if (ew == eye_w && eh == eye_h && cw == enc_w && ch == enc_h && f == fps && generation != 0 &&
		    (!vk_pool || (ten == ten_bit && full == full_range)))
			return;
		ten_bit = ten;
		full_range = full;
		eye_w = ew;
		eye_h = eh;
		enc_w = cw;
		enc_h = ch;
		fps = f;
		++generation;
		config_dirty = true;
	}
	write_begin();
	state->frame_period_ns = 1000000000LL / std::max(f, 1);
	write_end();
	wake('c');
}

void vr_server::set_foveation(int w, int h)
{
	{
		std::lock_guard lock(mutex);
		if (w == fovea_w && h == fovea_h)
			return;
		fovea_w = w;
		fovea_h = h;
		++generation;
		config_dirty = true;
	}
	wake('c');
}

void vr_server::set_gaze(bool valid, const float gaze[2][2])
{
	write_begin();
	state->gaze_valid = valid;
	std::memcpy(state->gaze, gaze, sizeof(state->gaze));
	write_end();
}

void vr_server::set_skeleton(const uint8_t * payload, size_t size)
{
	if (size < 4 || (size - 4) % ipc::skeleton_joint_bytes)
		return;
	const size_t joints = std::min((size - 4) / ipc::skeleton_joint_bytes, ipc::skeleton_max_joints);
	write_begin();
	state->skeleton_joint_count = uint32_t(joints);
	std::memcpy(&state->skeleton_changed_count, payload + size - 4, 4);
	std::memcpy(state->skeleton, payload, joints * ipc::skeleton_joint_bytes);
	write_end();
}

void vr_server::set_settings(const ipc::render_settings & settings)
{
	write_begin();
	state->settings = settings;
	write_end();
}

void vr_server::set_sink(std::shared_ptr<frame_sink> s)
{
	std::lock_guard lock(mutex);
	sink = std::move(s);
}
} // namespace od
