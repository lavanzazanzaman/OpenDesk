// Stand-in OpenXR app for the runtime interface: connects to the streamer's VR socket, renders the test scene
// (purple sky) with the headset's latest pose into the shared DMA-BUFs and submits frames at the stream's rate.
//   vr-test-client [SECONDS]
#include "frame_data.h"
#include "test_pattern.h"
#include "hardware_encoder.h" // default_render_node
#include "vr_client.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>

using namespace od;

static int64_t monotonic_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

int main(int argc, char ** argv)
{
	std::string error;
	auto client = vr_client::connect("vr-test-client", &error);
	if (!client)
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	if (argc > 2 && std::string(argv[2]) == "probe") // like Proton's helpers: connect, never render, leave
	{
		std::this_thread::sleep_for(std::chrono::seconds(std::atoi(argv[1])));
		return 0;
	}
	gbm_allocator alloc(vr_render_node());
	vr_test_pattern pattern(alloc);
	int seconds = argc > 1 ? std::atoi(argv[1]) : 1000000;
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	uint32_t generation = 0;
	int64_t frames = 0;
	auto next = std::chrono::steady_clock::now();
	while (std::chrono::steady_clock::now() < end)
	{
		if (!client->poll(0))
		{
			std::fprintf(stderr, "streamer went away\n");
			return 1;
		}
		if (client->generation() != generation)
		{
			generation = client->generation();
			const auto & c = client->config();
			std::printf("config %u: eye %ux%u, images %ux%u, %u slots, %u fps\n", c.generation, c.eye_width, c.eye_height, c.image.width,
			            c.image.height, c.slot_count, c.fps);
			std::fflush(stdout);
		}
		ipc::shared_state s{};
		client->read_state(s);
		auto slot = client->acquire(100);
		if (!slot)
			continue;
		frame_data::pose head = frame_data::head(s.frame_data);
		vr_render_info info;
		info.pose_index = frame_data::get<int32_t>(s.frame_data, 0);
		auto [x, y, z, w] = head.orientation;
		if (x == 0 && y == 0 && z == 0 && w == 0)
			head.orientation = {0, 0, 0, 1};
		float right[3] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)};
		for (int e = 0; e < 2; ++e)
		{
			info.eye[e].orientation = head.orientation;
			for (int k = 0; k < 3; ++k)
				info.eye[e].position[k] = head.position[k] + (e ? 1 : -1) * s.ipd / 2 * right[k];
			info.fov[e] = s.fov[e];
			const auto & img = client->eye_image(*slot, e);
			dmabuf_image target;
			target.fd = img.fd;
			target.width = img.desc.width;
			target.height = img.desc.height;
			target.stride = img.desc.stride;
			target.offset = img.desc.offset;
			target.fourcc = img.desc.fourcc;
			target.modifier = img.desc.modifier;
			pattern.render(target, e, info.eye[e], info.fov[e], 1);
		}
		if (!client->submit(*slot, info, monotonic_ns()))
			return 1;
		if (++frames % 360 == 0)
		{
			std::printf("%lld frames, pose frame %d, vr_active %u\n", (long long)frames, info.pose_index, s.vr_active);
			std::fflush(stdout);
		}
		next += std::chrono::nanoseconds(s.frame_period_ns > 0 ? s.frame_period_ns : 13888888);
		if (auto now = std::chrono::steady_clock::now(); next < now)
			next = now;
		std::this_thread::sleep_until(next);
	}
	std::printf("done: %lld frames\n", (long long)frames);
	return 0;
}
