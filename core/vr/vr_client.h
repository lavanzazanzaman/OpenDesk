// Client side of vr_ipc.h (runs inside the OpenXR app: libovr-opendesk, test clients). No Qt, no streamer internals.
#pragma once

#include "vr_ipc.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace od
{
class vr_client
{
public:
	struct image
	{
		int fd = -1; // owned by the client
		ipc::image_desc desc{};
	};

	static std::unique_ptr<vr_client> connect(const std::string & application, std::string * error);
	~vr_client();

	// handles pending server messages; waits up to timeout_ms for one. false once the streamer went away.
	bool poll(int timeout_ms);
	bool configured() const { return config_.generation != 0; }
	uint32_t generation() const { return config_.generation; }
	const ipc::config_msg & config() const { return config_; }
	uint32_t images_per_slot() const { return config_.foveated ? 4 : 2; }
	const image & eye_image(uint32_t slot, int eye) const { return images[images_per_slot() * slot + uint32_t(eye)]; }
	// foveated streaming only
	const image & fovea_image(uint32_t slot, int eye) const { return images[images_per_slot() * slot + 2 + uint32_t(eye)]; }
	void read_state(ipc::shared_state & out) const;

	// a slot whose images the streamer is done with; waits (handling messages) up to timeout_ms
	std::optional<uint32_t> acquire(int timeout_ms);
	bool submit(uint32_t slot, const vr_render_info &, int64_t display_time_ns); // reserve + send_submit
	// split for submitting from another thread once the GPU finished: reserve on the thread that polls/acquires,
	// send_submit (any thread) with the generation the frame was rendered for
	void reserve(uint32_t slot);
	bool send_submit(uint32_t generation, uint32_t slot, const vr_render_info &, int64_t display_time_ns) const;
	void haptics(int hand, float amplitude, float frequency, uint32_t duration_ms);
	void recenter();
	void render_thread(int tid); // the thread submitting frames, for "Boost game priority"
	void log(const std::string & text); // into the streamer's log
	void announce_rendering(); // the app renders: the streamer may switch the headset to VR

private:
	vr_client() = default;
	void drop_config();
	int fd = -1;
	ipc::config_msg config_{};
	std::vector<image> images;
	std::vector<bool> busy;
	const ipc::shared_state * state = nullptr;
	size_t state_size = 0;
};
} // namespace od
