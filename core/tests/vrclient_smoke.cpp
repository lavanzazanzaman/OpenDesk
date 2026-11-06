// Windows OpenVR smoke test without openvr_api.dll: loads an OpenVR runtime's vrclient_x64.dll (xrizer) the way
// openvr_api does - VRClientCoreFactory("IVRClientCore_003"), Init(Scene) - then the recommended render size and
// WaitGetPoses through the FnTable interfaces, like openvr-smoke on Linux.
//   vrclient-smoke PATH\TO\vrclient_x64.dll [SECONDS]
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
struct client_core // IVRClientCore_003 (virtual methods, declaration order)
{
	virtual int Init(int application_type, const char * startup_info) = 0;
	virtual void Cleanup() = 0;
	virtual int IsInterfaceVersionValid(const char * version) = 0;
	virtual void * GetGenericInterface(const char * version, int * error) = 0;
	virtual bool BIsHmdPresent() = 0;
};

struct pose
{
	float m[3][4];
	float velocity[3], angular[3];
	int result;
	bool valid, connected;
};

struct system_fn
{
	void (*GetRecommendedRenderTargetSize)(uint32_t * w, uint32_t * h);
};

struct compositor_fn
{
	void (*SetTrackingSpace)(int);
	int (*GetTrackingSpace)();
	int (*WaitGetPoses)(pose * render, uint32_t render_count, pose * game, uint32_t game_count);
};
} // namespace

int main(int argc, char ** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 2)
	{
		std::printf("usage: vrclient-smoke vrclient_x64.dll [SECONDS]\n");
		return 2;
	}
	const int seconds = argc > 2 ? std::atoi(argv[2]) : 3;
	HMODULE lib = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!lib)
	{
		std::printf("FAIL  cannot load %s (error %lu)\n", argv[1], GetLastError());
		return 1;
	}
	using factory_fn = void * (*)(const char *, int *);
	auto factory = reinterpret_cast<factory_fn>(reinterpret_cast<void *>(GetProcAddress(lib, "VRClientCoreFactory")));
	int error = 0;
	auto * core = factory ? static_cast<client_core *>(factory("IVRClientCore_003", &error)) : nullptr;
	if (!core)
	{
		std::printf("FAIL  VRClientCoreFactory (error %d)\n", error);
		return 1;
	}
	error = core->Init(1 /* VRApplication_Scene */, nullptr);
	std::printf("%s  Init (error %d)\n", error ? "FAIL" : "PASS", error);
	if (error)
		return 1;
	auto * system = static_cast<system_fn *>(core->GetGenericInterface("FnTable:IVRSystem_022", &error));
	auto * compositor = static_cast<compositor_fn *>(core->GetGenericInterface("FnTable:IVRCompositor_028", &error));
	if (!system || !compositor)
	{
		std::printf("FAIL  interfaces IVRSystem_022 / IVRCompositor_028 (error %d)\n", error);
		core->Cleanup();
		return 1;
	}
	uint32_t w = 0, h = 0;
	system->GetRecommendedRenderTargetSize(&w, &h);
	std::printf("%s  recommended render size %ux%u\n", w && h ? "PASS" : "FAIL", w, h);
	int frames = 0, valid = 0;
	pose poses[16] = {};
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (std::chrono::steady_clock::now() < end)
	{
		compositor->WaitGetPoses(poses, 16, nullptr, 0);
		++frames;
		valid += poses[0].valid;
	}
	std::printf("%s  WaitGetPoses: %d frames, HMD pose valid in %d\n", frames > 10 ? "PASS" : "FAIL", frames, valid);
	core->Cleanup();
	return w && h && frames > 10 ? 0 : 1;
}
