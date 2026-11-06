// OpenVR smoke test: VR_Init through whatever openvrpaths.vrpath registers (with the streamer: xrizer on OpenDesk XR),
// the recommended render size, and HMD poses from IVRCompositor::WaitGetPoses.
//   openvr-smoke SECONDS [path/to/libopenvr_api.so]   (default: $OPENDESK_OPENVR_API, SteamVR's copy or the Steam client's)
// Only the few interface slots used here are declared (function tables of IVRSystem_022 / IVRCompositor_028).
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include <string>

namespace
{
struct pose
{
	float m[3][4];
	float velocity[3], angular[3];
	int result;
	bool valid, connected;
};
static_assert(sizeof(pose) == 80);

// OpenVR's C function tables ("FnTable:<interface>"): the methods in declaration order
struct hidden_area_mesh
{
	const float * vertices; // HmdVector2_t triangles, [0,1] in the eye image
	uint32_t triangles;
};

struct system_fn
{
	void (*GetRecommendedRenderTargetSize)(uint32_t * w, uint32_t * h);
	void * unused_1_31[31];
	hidden_area_mesh (*GetHiddenAreaMesh)(int eye, int type); // slot 32 of IVRSystem_022; type 0 = standard (hidden)
};
static_assert(offsetof(system_fn, GetHiddenAreaMesh) == 32 * sizeof(void *));

struct compositor_fn
{
	void (*SetTrackingSpace)(int);
	int (*GetTrackingSpace)();
	int (*WaitGetPoses)(pose * render, uint32_t render_count, pose * game, uint32_t game_count);
};

using init_fn = uint32_t (*)(int * error, int type, const char * startup_info);
using get_fn = void * (*)(const char * version, int * error);
using shutdown_fn = void (*)();
} // namespace

int main(int argc, char ** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
	std::string lib = argc > 2 ? argv[2] : getenv("OPENDESK_OPENVR_API") ? getenv("OPENDESK_OPENVR_API") : "";
	if (lib.empty() && getenv("HOME"))
	{
		lib = std::string(getenv("HOME")) + "/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/libopenvr_api.so";
		if (access(lib.c_str(), R_OK) != 0) // no SteamVR: the Steam client's copy
			lib = std::string(getenv("HOME")) + "/.local/share/Steam/ubuntu12_64/libopenvr_api.so";
	}
	void * api = dlopen(lib.c_str(), RTLD_NOW);
	if (!api)
	{
		std::printf("SKIP  no OpenVR loader (%s)\n", dlerror());
		return 77;
	}
	auto init = reinterpret_cast<init_fn>(dlsym(api, "VR_InitInternal2"));
	auto get = reinterpret_cast<get_fn>(dlsym(api, "VR_GetGenericInterface"));
	auto shutdown = reinterpret_cast<shutdown_fn>(dlsym(api, "VR_ShutdownInternal"));
	if (!init || !get || !shutdown)
	{
		std::printf("FAIL  loader lacks the VR_* entry points\n");
		return 1;
	}
	int error = 0;
	init(&error, 1 /* VRApplication_Scene */, nullptr);
	std::printf("%s  VR_Init (error %d)\n", error ? "FAIL" : "PASS", error);
	if (error)
		return 1;
	auto * system = static_cast<system_fn *>(get("FnTable:IVRSystem_022", &error));
	std::printf("      IVRSystem_022 %p (error %d)\n", (void *)system, error);
	auto * compositor = static_cast<compositor_fn *>(get("FnTable:IVRCompositor_028", &error));
	std::printf("      IVRCompositor_028 %p (error %d)\n", (void *)compositor, error);
	if (!system || !compositor)
	{
		std::printf("FAIL  interfaces IVRSystem_022 / IVRCompositor_028\n");
		shutdown();
		return 1;
	}
	uint32_t w = 0, h = 0;
	system->GetRecommendedRenderTargetSize(&w, &h);
	std::printf("%s  recommended render size %ux%u\n", w && h ? "PASS" : "FAIL", w, h);
	std::printf("      WaitGetPoses...\n");
	int frames = 0, valid = 0;
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	pose poses[16] = {};
	while (std::chrono::steady_clock::now() < end)
	{
		compositor->WaitGetPoses(poses, 16, nullptr, 0);
		++frames;
		valid += poses[0].valid;
	}
	std::printf("%s  WaitGetPoses: %d frames, HMD pose valid in %d (head at %.2f %.2f %.2f)\n", frames > 10 && valid ? "PASS" : "FAIL", frames,
	            valid, poses[0].m[0][3], poses[0].m[1][3], poses[0].m[2][3]);
	// the FOV stencil reaches OpenVR games (xrizer -> OpenDesk XR's XR_KHR_visibility_mask): triangles over the image corners,
	// none over the centre
	bool mask_ok = true;
	const char * no_stencil = std::getenv("OPENDESK_TEST_NO_STENCIL"); // "Use Fov Stencil" off: no mesh is the right answer
	const bool off = no_stencil && *no_stencil;
	for (int eye = 0; eye < 2 && !off; ++eye)
	{
		hidden_area_mesh m = system->GetHiddenAreaMesh(eye, 0);
		auto covers = [&](float px, float py) {
			for (uint32_t t = 0; t < m.triangles; ++t)
			{
				const float * v = m.vertices + 6 * t;
				auto cross = [&](int a, int b) { return (v[2 * b] - v[2 * a]) * (py - v[2 * a + 1]) - (v[2 * b + 1] - v[2 * a + 1]) * (px - v[2 * a]); };
				float area = (v[2] - v[0]) * (v[5] - v[1]) - (v[3] - v[1]) * (v[4] - v[0]);
				if (std::fabs(area) < 1e-9f)
					continue;
				float d1 = cross(0, 1), d2 = cross(1, 2), d3 = cross(2, 0);
				if (!((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0)))
					return true;
			}
			return false;
		};
		bool ok_eye = m.triangles > 0 && covers(0.02f, 0.02f) && covers(0.98f, 0.98f) && !covers(0.5f, 0.5f);
		std::printf("%s  GetHiddenAreaMesh eye %d: %u triangles over the corners, not the centre\n", ok_eye ? "PASS" : "FAIL", eye, m.triangles);
		mask_ok &= ok_eye;
	}
	if (off)
	{
		hidden_area_mesh m = system->GetHiddenAreaMesh(0, 0);
		mask_ok = m.triangles == 0;
		std::printf("%s  stencil off: GetHiddenAreaMesh empty (%u triangles)\n", mask_ok ? "PASS" : "FAIL", m.triangles);
	}
	shutdown();
	bool ok = w && h && frames > 10 && valid && mask_ok;
	std::printf(ok ? "ALL PASS\n" : "FAILED\n");
	return ok ? 0 : 1;
}
