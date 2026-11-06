#include "gamepad.h"

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <linux/uinput.h>

#include <algorithm>
#include <cstdlib>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

namespace od
{
namespace
{
// GamepadButtonFlags (Upstream.Interfaces)
constexpr uint16_t dpad_up = 1, dpad_down = 2, dpad_left = 4, dpad_right = 8, start = 0x10, back = 0x20,
                   left_thumb = 0x40, right_thumb = 0x80, left_shoulder = 0x100, right_shoulder = 0x200,
                   guide = 0x400, button_a = 0x1000, button_b = 0x2000, button_x = 0x4000, button_y = 0x8000;

struct raw_state
{
	uint16_t buttons;
	uint8_t left_trigger, right_trigger;
	int16_t lx, ly, rx, ry;
	uint8_t special; // bit 1: touchpad click of a paired DualShock/DualSense
};

raw_state parse(const uint8_t * p)
{
	return {read_le<uint16_t>(p), p[2], p[3], read_le<int16_t>(p + 4), read_le<int16_t>(p + 6),
	        read_le<int16_t>(p + 8), read_le<int16_t>(p + 10), p[12]};
}
} // namespace

bool gamepad_state_active(const uint8_t * p)
{
	raw_state s = parse(p);
	return s.buttons || s.left_trigger || s.right_trigger || s.lx || s.ly || s.rx || s.ry || s.special;
}

virtual_gamepad::virtual_gamepad(gamepad_model m, rumble_callback cb) :
        kind(m), on_rumble(std::move(cb))
{
	libevdev * d = libevdev_new();
	libevdev_set_id_bustype(d, BUS_USB);
	if (kind == gamepad_model::dualshock4)
	{
		// same identity/layout as hid-playstation's DualShock 4
		libevdev_set_name(d, "Sony Interactive Entertainment Wireless Controller");
		libevdev_set_id_vendor(d, 0x054c);
		libevdev_set_id_product(d, 0x09cc);
		libevdev_set_id_version(d, 0x8111);
		for (int b: {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START,
		             BTN_MODE, BTN_THUMBL, BTN_THUMBR})
			libevdev_enable_event_code(d, EV_KEY, b, nullptr);
		input_absinfo stick{.value = 128, .minimum = 0, .maximum = 255, .fuzz = 0, .flat = 0};
		for (int a: {ABS_X, ABS_Y, ABS_RX, ABS_RY})
			libevdev_enable_event_code(d, EV_ABS, a, &stick);
	}
	else
	{
		// same identity/layout as the kernel's xpad driver for an Xbox 360 pad
		libevdev_set_name(d, "Microsoft X-Box 360 pad");
		libevdev_set_id_vendor(d, 0x045e);
		libevdev_set_id_product(d, 0x028e);
		libevdev_set_id_version(d, 0x0110);
		for (int b: {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR})
			libevdev_enable_event_code(d, EV_KEY, b, nullptr);
		input_absinfo stick{.value = 0, .minimum = -32768, .maximum = 32767, .fuzz = 16, .flat = 128};
		for (int a: {ABS_X, ABS_Y, ABS_RX, ABS_RY})
			libevdev_enable_event_code(d, EV_ABS, a, &stick);
	}
	input_absinfo trigger{.value = 0, .minimum = 0, .maximum = 255};
	input_absinfo hat{.value = 0, .minimum = -1, .maximum = 1};
	libevdev_enable_event_code(d, EV_ABS, ABS_Z, &trigger);
	libevdev_enable_event_code(d, EV_ABS, ABS_RZ, &trigger);
	libevdev_enable_event_code(d, EV_ABS, ABS_HAT0X, &hat);
	libevdev_enable_event_code(d, EV_ABS, ABS_HAT0Y, &hat);
	// rumble: FF_RUMBLE (+ the periodic/constant types games commonly probe for)
	for (int f: {FF_RUMBLE, FF_PERIODIC, FF_SQUARE, FF_TRIANGLE, FF_SINE, FF_GAIN})
		libevdev_enable_event_code(d, EV_FF, f, nullptr);

	int err = libevdev_uinput_create_from_device(d, LIBEVDEV_UINPUT_OPEN_MANAGED, &dev);
	libevdev_free(d);
	if (err)
		throw std::runtime_error("cannot create the virtual gamepad (uinput)");
	ff_thread = std::thread(&virtual_gamepad::ff_loop, this);
}

virtual_gamepad::~virtual_gamepad()
{
	quit = true;
	if (ff_thread.joinable())
		ff_thread.join();
	if (dev)
		libevdev_uinput_destroy(dev);
	if (on_rumble)
		on_rumble(0, 0);
}

void virtual_gamepad::apply(const uint8_t * p)
{
	raw_state s = parse(p);
	const bool ds4 = kind == gamepad_model::dualshock4;
	std::array<int, 16> v{};
	int i = 0;
	auto btn = [&](uint16_t flag) { return (s.buttons & flag) ? 1 : 0; };
	// XInput-style Y is up-positive; evdev is down-positive
	auto stick = [&](int16_t value, bool invert) {
		int x = invert ? -std::max<int>(value, -32767) : value;
		return ds4 ? std::clamp((x + 32768) >> 8, 0, 255) : x;
	};
	const int codes[] = {ds4 ? BTN_SOUTH : BTN_A, ds4 ? BTN_EAST : BTN_B, ds4 ? BTN_WEST : BTN_X, ds4 ? BTN_NORTH : BTN_Y,
	                     BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
	const int pressed[] = {btn(button_a), btn(button_b), btn(button_x), btn(button_y), btn(left_shoulder),
	                       btn(right_shoulder), btn(back) | ((s.special & 2) ? 1 : 0), btn(start), btn(guide),
	                       btn(left_thumb), btn(right_thumb)};
	for (int k = 0; k < 11; ++k)
		v[i++] = pressed[k];
	v[i++] = s.left_trigger;
	v[i++] = s.right_trigger;
	int hx = btn(dpad_right) - btn(dpad_left), hy = btn(dpad_down) - btn(dpad_up);

	bool dirty = false;
	auto emit = [&](unsigned type, unsigned code, int value, int slot) {
		if (!first && last[slot] == value)
			return;
		last[slot] = value;
		libevdev_uinput_write_event(dev, type, code, value);
		dirty = true;
	};
	for (int k = 0; k < 11; ++k)
		emit(EV_KEY, codes[k], v[k], k);
	emit(EV_ABS, ABS_Z, v[11], 11);
	emit(EV_ABS, ABS_RZ, v[12], 12);
	if (ds4)
	{
		emit(EV_KEY, BTN_TL2, s.left_trigger > 0, 13);
		emit(EV_KEY, BTN_TR2, s.right_trigger > 0, 14);
	}
	// sticks and hat: always written (cheap, and keeps the axis state exact)
	libevdev_uinput_write_event(dev, EV_ABS, ABS_X, stick(s.lx, false));
	libevdev_uinput_write_event(dev, EV_ABS, ABS_Y, stick(s.ly, true));
	libevdev_uinput_write_event(dev, EV_ABS, ABS_RX, stick(s.rx, false));
	libevdev_uinput_write_event(dev, EV_ABS, ABS_RY, stick(s.ry, true));
	libevdev_uinput_write_event(dev, EV_ABS, ABS_HAT0X, hx);
	libevdev_uinput_write_event(dev, EV_ABS, ABS_HAT0Y, hy);
	libevdev_uinput_write_event(dev, EV_SYN, SYN_REPORT, 0);
	(void)dirty;
	first = false;
}

void virtual_gamepad::update_rumble()
{
	float strong = 0, weak = 0;
	{
		std::lock_guard lock(ff_mutex);
		for (const auto & [id, e]: effects)
			if (e.playing)
			{
				strong = std::max(strong, e.strong / 65535.f);
				weak = std::max(weak, e.weak / 65535.f);
			}
	}
	if (on_rumble)
		on_rumble(strong, weak);
}

void virtual_gamepad::ff_loop()
{
	// uinput force feedback: answer UI_FF_UPLOAD / UI_FF_ERASE, track EV_FF play/stop
	int fd = libevdev_uinput_get_fd(dev);
	while (!quit)
	{
		pollfd pfd{fd, POLLIN, 0};
		if (poll(&pfd, 1, 100) <= 0)
			continue;
		input_event ev;
		if (read(fd, &ev, sizeof(ev)) != sizeof(ev))
			continue;
		if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD)
		{
			uinput_ff_upload up{};
			up.request_id = uint32_t(ev.value);
			if (ioctl(fd, UI_BEGIN_FF_UPLOAD, &up) < 0)
				continue;
			{
				std::lock_guard lock(ff_mutex);
				effect & e = effects[up.effect.id];
				if (up.effect.type == FF_RUMBLE)
				{
					e.strong = up.effect.u.rumble.strong_magnitude;
					e.weak = up.effect.u.rumble.weak_magnitude;
				}
				else // periodic & co: use the magnitude for both motors
					e.strong = e.weak = uint16_t(std::min<int>(std::abs(up.effect.u.periodic.magnitude) * 2, 65535));
			}
			up.retval = 0;
			ioctl(fd, UI_END_FF_UPLOAD, &up);
		}
		else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE)
		{
			uinput_ff_erase er{};
			er.request_id = uint32_t(ev.value);
			if (ioctl(fd, UI_BEGIN_FF_ERASE, &er) < 0)
				continue;
			{
				std::lock_guard lock(ff_mutex);
				effects.erase(int(er.effect_id));
			}
			er.retval = 0;
			ioctl(fd, UI_END_FF_ERASE, &er);
			update_rumble();
		}
		else if (ev.type == EV_FF && ev.code != FF_GAIN)
		{
			{
				std::lock_guard lock(ff_mutex);
				if (auto it = effects.find(ev.code); it != effects.end())
					it->second.playing = ev.value > 0;
			}
			update_rumble();
		}
	}
}
} // namespace od
