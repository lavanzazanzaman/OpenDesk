// Runs the VR server alone with a sink that only counts frames (no headset, no encoder): pairs with vr-test-client to
// check the transport (Windows: named pipe + NT handles).
//   vr-server-test [SECONDS]
#include "vr_server.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace od;

struct counting_sink : vr_server::frame_sink
{
	std::atomic<int> frames{0};
	void submit(std::vector<std::shared_ptr<const dmabuf_image>> images, const vr_render_info &, std::function<void()> done) override
	{
		if (frames++ == 0)
			std::printf("first frame: %zu images, %ux%u\n", images.size(), images[0]->width, images[0]->height);
		done();
	}
};

int main(int argc, char ** argv)
{
	int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
	vr_server::callbacks cb;
	cb.log = [](std::string l) { std::printf("%s\n", l.c_str()); std::fflush(stdout); };
	cb.client = [](bool connected, std::string app) { std::printf("client %s: %s\n", connected ? "selected" : "left", app.c_str()); };
	auto sink = std::make_shared<counting_sink>();
	vr_server server({}, cb);
	server.set_sink(sink);
	fov_port fov[2]{};
	server.set_display(true, true, 0.064f, fov, 0);
	std::this_thread::sleep_for(std::chrono::seconds(seconds));
	std::printf("frames received: %d\n", sink->frames.load());
	server.set_sink(nullptr);
	return sink->frames.load() > 0 ? 0 : 1;
}
