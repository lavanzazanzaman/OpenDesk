#include "gamepad_win.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace od
{
namespace
{
struct raw_state
{
	uint16_t buttons;
	uint8_t left_trigger, right_trigger;
	int16_t lx, ly, rx, ry;
	uint8_t special;
};

raw_state parse(const uint8_t * p)
{
	return {read_le<uint16_t>(p), p[2], p[3], read_le<int16_t>(p + 4), read_le<int16_t>(p + 6),
	        read_le<int16_t>(p + 8), read_le<int16_t>(p + 10), p[12]};
}

// ViGEmBus client ABI (the public C header, kept here so the DLL stays optional).
struct XUSB_REPORT
{
	USHORT wButtons;
	BYTE bLeftTrigger;
	BYTE bRightTrigger;
	SHORT sThumbLX;
	SHORT sThumbLY;
	SHORT sThumbRX;
	SHORT sThumbRY;
};
struct vigem_client;
struct vigem_target;
using pfn_alloc = vigem_client *(*)();
using pfn_free = void (*)(vigem_client *);
using pfn_connect = int (*)(vigem_client *);
using pfn_disconnect = void (*)(vigem_client *);
using pfn_target_alloc = vigem_target *(*)();
using pfn_target_free = void (*)(vigem_target *);
using pfn_target_add = int (*)(vigem_client *, vigem_target *);
using pfn_target_remove = int (*)(vigem_client *, vigem_target *);
using pfn_update = int (*)(vigem_client *, vigem_target *, XUSB_REPORT);
} // namespace

struct virtual_gamepad::impl
{
	HMODULE dll = nullptr;
	vigem_client * client = nullptr;
	vigem_target * target = nullptr;
	pfn_free free_client = nullptr;
	pfn_disconnect disconnect = nullptr;
	pfn_target_free free_target = nullptr;
	pfn_target_remove remove_target = nullptr;
	pfn_update update = nullptr;

	~impl()
	{
		if (client && target && remove_target)
			remove_target(client, target);
		if (target && free_target)
			free_target(target);
		if (client && disconnect)
			disconnect(client);
		if (client && free_client)
			free_client(client);
		if (dll)
			FreeLibrary(dll);
	}
};

bool gamepad_state_active(const uint8_t * p)
{
	raw_state s = parse(p);
	return s.buttons || s.left_trigger || s.right_trigger || s.lx || s.ly || s.rx || s.ry || s.special;
}

virtual_gamepad::virtual_gamepad(gamepad_model m, rumble_callback cb) :
        kind(m), on_rumble(std::move(cb)), state(std::make_unique<impl>())
{
	auto & s = *state;
	s.dll = LoadLibraryA("ViGEmClient.dll");
	if (!s.dll)
		return;
	auto load = [&](const char * name) { return GetProcAddress(s.dll, name); };
	auto alloc = reinterpret_cast<pfn_alloc>(load("vigem_alloc"));
	auto connect = reinterpret_cast<pfn_connect>(load("vigem_connect"));
	auto target_alloc = reinterpret_cast<pfn_target_alloc>(load("vigem_target_x360_alloc"));
	auto target_add = reinterpret_cast<pfn_target_add>(load("vigem_target_add"));
	s.free_client = reinterpret_cast<pfn_free>(load("vigem_free"));
	s.disconnect = reinterpret_cast<pfn_disconnect>(load("vigem_disconnect"));
	s.free_target = reinterpret_cast<pfn_target_free>(load("vigem_target_free"));
	s.remove_target = reinterpret_cast<pfn_target_remove>(load("vigem_target_remove"));
	s.update = reinterpret_cast<pfn_update>(load("vigem_target_x360_update"));
	if (!alloc || !connect || !target_alloc || !target_add || !s.free_client || !s.disconnect || !s.free_target ||
	    !s.remove_target || !s.update)
		return;
	s.client = alloc();
	if (!s.client || connect(s.client) != 0)
		return;
	s.target = target_alloc();
	if (!s.target || target_add(s.client, s.target) != 0)
	{
		if (s.target)
			s.free_target(s.target);
		s.target = nullptr;
	}
}

virtual_gamepad::~virtual_gamepad() = default;

void virtual_gamepad::apply(const uint8_t * p)
{
	if (!state || !state->client || !state->target)
		return;
	const raw_state s = parse(p);
	XUSB_REPORT report{};
	report.wButtons = s.buttons;
	report.bLeftTrigger = s.left_trigger;
	report.bRightTrigger = s.right_trigger;
	report.sThumbLX = s.lx;
	report.sThumbLY = s.ly;
	report.sThumbRX = s.rx;
	report.sThumbRY = s.ry;
	state->update(state->client, state->target, report);
	first = false;
}
} // namespace od
