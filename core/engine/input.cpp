#include "input.h"

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace od
{
namespace
{
constexpr size_t input_end = 118; // InputState + LockStates
constexpr size_t left_pointer = 12, right_pointer = 22; // RawPointerState: 5 flags, pad, i16 X, i16 Y
constexpr size_t keyboard_offset = 32, key_slots = 8, key_slot_size = 3; // {u16 ScanCode, u8 KeyStateFlags}
constexpr size_t mouse_offset = 60; // flags @60..65, f32 X @68, f32 Y @72, i8 ScrollX @76, i8 ScrollY @77
constexpr uint8_t key_down = 1, key_pressed = 2;
constexpr int scroll_hires_per_unit = 15; // headset scroll halves each packet; tuned by feel

// PC set-1 scancodes (Upstream.Interfaces.ScanCodes) -> Linux KEY_*; 1..83 are identical
int scancode_to_key(uint16_t sc)
{
	if (sc >= 1 && sc <= 83)
		return sc;
	static const std::map<uint16_t, int> table{
	        {84, KEY_SYSRQ}, {86, KEY_102ND}, {87, KEY_F11}, {88, KEY_F12}, {112, KEY_F21}, {115, KEY_F24},
	        {121, KEY_AGAIN}, {123, KEY_CUT}, {125, KEY_PASTE},
	        {0xE01C, KEY_KPENTER}, {0xE01D, KEY_RIGHTCTRL}, {0xE035, KEY_KPSLASH}, {0xE038, KEY_RIGHTALT},
	        {0xE045, KEY_PAUSE}, {0xE047, KEY_HOME}, {0xE048, KEY_UP}, {0xE049, KEY_PAGEUP}, {0xE04B, KEY_LEFT},
	        {0xE04D, KEY_RIGHT}, {0xE04F, KEY_END}, {0xE050, KEY_DOWN}, {0xE051, KEY_PAGEDOWN},
	        {0xE052, KEY_INSERT}, {0xE053, KEY_DELETE}, {0xE05B, KEY_LEFTMETA}, {0xE05C, KEY_RIGHTMETA},
	        {0xE05D, KEY_COMPOSE}};
	auto it = table.find(sc);
	return it == table.end() ? -1 : it->second;
}

libevdev_uinput * create(libevdev * dev)
{
	libevdev_uinput * ui = nullptr;
	int err = libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &ui);
	libevdev_free(dev);
	if (err)
		throw std::runtime_error("cannot create uinput device (is /dev/uinput writable?)");
	return ui;
}

void write(libevdev_uinput * ui, unsigned type, unsigned code, int value)
{
	libevdev_uinput_write_event(ui, type, code, value);
}

void syn(libevdev_uinput * ui)
{
	libevdev_uinput_write_event(ui, EV_SYN, SYN_REPORT, 0);
}
} // namespace

input_injector::input_injector()
{
	libevdev * k = libevdev_new();
	libevdev_set_name(k, "OpenDesk Keyboard/Mouse");
	libevdev_set_id_vendor(k, 0x1d6b);
	libevdev_set_id_product(k, 0x5645);
	libevdev_set_id_bustype(k, BUS_VIRTUAL);
	for (int sc = 1; sc <= 0xE05D; ++sc)
		if (int key = scancode_to_key(uint16_t(sc)); key >= 0)
			libevdev_enable_event_code(k, EV_KEY, key, nullptr);
	for (int b: {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA})
		libevdev_enable_event_code(k, EV_KEY, b, nullptr);
	for (int r: {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES})
		libevdev_enable_event_code(k, EV_REL, r, nullptr);
	kbd_mouse_dev = create(k);
}

bool input_injector::set_layout(const input_layout & l)
{
	if (l == current || l.width <= 0 || l.height <= 0 || l.screens.empty())
		return false;
	destroy_pointer_devices();
	current = l;

	// Absolute pointer (like a QEMU tablet): udev tags it ID_INPUT_MOUSE, KWin maps it onto the whole desktop
	libevdev * p = libevdev_new();
	libevdev_set_name(p, "OpenDesk Pointer");
	libevdev_set_id_vendor(p, 0x1d6b);
	libevdev_set_id_product(p, 0x5644);
	libevdev_set_id_bustype(p, BUS_VIRTUAL);
	for (int b: {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE})
		libevdev_enable_event_code(p, EV_KEY, b, nullptr);
	input_absinfo ax{.value = 0, .minimum = 0, .maximum = l.width - 1};
	input_absinfo ay{.value = 0, .minimum = 0, .maximum = l.height - 1};
	libevdev_enable_event_code(p, EV_ABS, ABS_X, &ax);
	libevdev_enable_event_code(p, EV_ABS, ABS_Y, &ay);
	pointer_dev = create(p);

	// Direct touchscreen per screen (INPUT_PROP_DIRECT, multitouch protocol B): KWin maps each onto one output
	for (size_t i = 0; i < l.screens.size(); ++i)
	{
		const rect & b = l.screens[i].bounds;
		libevdev * t = libevdev_new();
		std::string name = i == 0 ? "OpenDesk Touch" : "OpenDesk Touch " + std::to_string(i + 1);
		libevdev_set_name(t, name.c_str());
		libevdev_set_id_vendor(t, 0x1d6b);
		libevdev_set_id_product(t, 0x5646);
		libevdev_set_id_bustype(t, BUS_VIRTUAL);
		libevdev_enable_property(t, INPUT_PROP_DIRECT);
		for (int k: {BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP})
			libevdev_enable_event_code(t, EV_KEY, k, nullptr);
		input_absinfo slot{.value = 0, .minimum = 0, .maximum = 1};
		input_absinfo tid{.value = 0, .minimum = 0, .maximum = 65535};
		input_absinfo tx{.value = 0, .minimum = 0, .maximum = std::max(b.width - 1, 1)};
		input_absinfo ty{.value = 0, .minimum = 0, .maximum = std::max(b.height - 1, 1)};
		libevdev_enable_event_code(t, EV_ABS, ABS_X, &tx);
		libevdev_enable_event_code(t, EV_ABS, ABS_Y, &ty);
		libevdev_enable_event_code(t, EV_ABS, ABS_MT_SLOT, &slot);
		libevdev_enable_event_code(t, EV_ABS, ABS_MT_TRACKING_ID, &tid);
		libevdev_enable_event_code(t, EV_ABS, ABS_MT_POSITION_X, &tx);
		libevdev_enable_event_code(t, EV_ABS, ABS_MT_POSITION_Y, &ty);
		touch_devs.push_back({create(t), b});
	}
	return true;
}

std::vector<std::string> input_injector::touch_devnodes() const
{
	std::vector<std::string> nodes;
	for (const auto & t: touch_devs)
	{
		const char * n = libevdev_uinput_get_devnode(t.dev);
		nodes.push_back(n ? n : "");
	}
	return nodes;
}

void input_injector::destroy_pointer_devices()
{
	release_touches();
	for (auto & t: touch_devs)
		libevdev_uinput_destroy(t.dev);
	touch_devs.clear();
	if (pointer_dev)
	{
		static constexpr int codes[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
		for (int b = 0; b < 3; ++b)
			if (pointer_buttons[b])
				write(pointer_dev, EV_KEY, codes[b], 0);
		syn(pointer_dev);
		libevdev_uinput_destroy(pointer_dev);
		pointer_dev = nullptr;
	}
	pointer_buttons = {};
	touch_right = {};
	last_x = last_y = {-1, -1};
	current = {};
}

void input_injector::set_touch_mode(bool on)
{
	if (on == touch_mode)
		return;
	if (touch_mode)
		release_touches();
	else if (pointer_dev)
	{
		static constexpr int codes[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
		for (int b = 0; b < 3; ++b) // release mouse buttons held by the pointer
			if (pointer_buttons[b])
				write(pointer_dev, EV_KEY, codes[b], 0);
		pointer_buttons = {};
		syn(pointer_dev);
	}
	touch_mode = on;
}

void input_injector::release_touches()
{
	for (auto & t: touch_devs)
	{
		bool any = false;
		for (int i = 0; i < 2; ++i)
			if (t.touching[i])
			{
				write(t.dev, EV_ABS, ABS_MT_SLOT, i);
				write(t.dev, EV_ABS, ABS_MT_TRACKING_ID, -1);
				t.touching[i] = false;
				any = true;
			}
		if (any)
		{
			write(t.dev, EV_KEY, BTN_TOUCH, 0);
			write(t.dev, EV_KEY, BTN_TOOL_FINGER, 0);
			write(t.dev, EV_KEY, BTN_TOOL_DOUBLETAP, 0);
			syn(t.dev);
		}
	}
	finger_screen = {-1, -1};
}

input_injector::~input_injector()
{
	if (kbd_mouse_dev)
	{
		for (int key: keys_down)
			write(kbd_mouse_dev, EV_KEY, key, 0);
		syn(kbd_mouse_dev);
		libevdev_uinput_destroy(kbd_mouse_dev);
	}
	destroy_pointer_devices();
}

void input_injector::apply(const bytes & fd)
{
	if (fd.size() < input_end)
		return;
	if (pointer_dev)
	{
		if (touch_mode)
			touch_pointers(fd.data());
		else
			pointers(fd.data());
	}
	mouse(fd.data());
	keyboard(fd.data());
}

void input_injector::pointers(const uint8_t * fd)
{
	std::array<bool, 3> buttons{};
	bool dirty = false;
	int i = 0;
	// the real mouse beats a resting laser: a controller held still jitters by a few pixels every frame, which would
	// pull the cursor back to the laser all the time. The laser takes the cursor only when it moves deliberately (more
	// than the deadzone from where it last put it), while one of its buttons is held, or right before one changes (clicks
	// land at the laser).
	const int deadzone = std::max(4, current.width / 400);
	for (size_t off: {left_pointer, right_pointer})
	{
		const uint8_t * p = fd + off;
		bool active = p[0], overlay = p[1];
		if (!active || overlay) // over upstream's own UI: don't move the PC cursor
		{
			last_x[i] = last_y[i] = -1;
			++i;
			continue;
		}
		int x = std::clamp<int>(read_le<int16_t>(p + 6), 0, current.width - 1);
		int y = std::clamp<int>(read_le<int16_t>(p + 8), 0, current.height - 1);
		bool clicking = false, holding = false; // a button changes / is held (dragging: follow exactly)
		for (int b = 0; b < 3; ++b)
		{
			clicking |= bool(p[2 + b]) != pointer_buttons[b];
			holding |= bool(p[2 + b]);
		}
		bool moved = last_x[i] < 0 || holding || std::abs(x - last_x[i]) > deadzone || std::abs(y - last_y[i]) > deadzone;
		if ((moved || clicking) && (x != last_x[i] || y != last_y[i]))
		{
			last_x[i] = x;
			last_y[i] = y;
			write(pointer_dev, EV_ABS, ABS_X, x);
			write(pointer_dev, EV_ABS, ABS_Y, y);
			dirty = true;
		}
		for (int b = 0; b < 3; ++b)
			buttons[b] = buttons[b] || p[2 + b];
		++i;
	}
	static constexpr int codes[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
	for (int b = 0; b < 3; ++b)
		if (buttons[b] != pointer_buttons[b])
		{
			write(pointer_dev, EV_KEY, codes[b], buttons[b]);
			dirty = true;
		}
	pointer_buttons = buttons;
	if (dirty)
		syn(pointer_dev);
}

void input_injector::touch_pointers(const uint8_t * fd)
{
	// trigger (LeftDown) = finger down at the laser position; drag while held; hovering does nothing.
	// B/second button (RightDown) = right click at the laser position through the absolute pointer.
	const size_t n = touch_devs.size();
	std::vector<int> fingers_before(n);
	std::vector<bool> dirty(n);
	for (size_t s = 0; s < n; ++s)
		fingers_before[s] = int(touch_devs[s].touching[0]) + int(touch_devs[s].touching[1]);

	auto finger = [&](int screen, int slot, bool down, int gx, int gy) {
		touch_device & t = touch_devs[size_t(screen)];
		int x = std::clamp(gx - t.bounds.x, 0, std::max(t.bounds.width - 1, 0));
		int y = std::clamp(gy - t.bounds.y, 0, std::max(t.bounds.height - 1, 0));
		if (down == t.touching[slot] && (!down || (x == t.x[slot] && y == t.y[slot])))
			return;
		write(t.dev, EV_ABS, ABS_MT_SLOT, slot);
		if (down && !t.touching[slot])
			write(t.dev, EV_ABS, ABS_MT_TRACKING_ID, next_tracking_id++ & 0xffff);
		if (down)
		{
			write(t.dev, EV_ABS, ABS_MT_POSITION_X, x);
			write(t.dev, EV_ABS, ABS_MT_POSITION_Y, y);
			t.x[slot] = x;
			t.y[slot] = y;
		}
		else
			write(t.dev, EV_ABS, ABS_MT_TRACKING_ID, -1);
		t.touching[slot] = down;
		dirty[size_t(screen)] = true;
	};

	for (int i = 0; i < 2; ++i)
	{
		const uint8_t * p = fd + (i == 0 ? left_pointer : right_pointer);
		bool active = p[0] && !p[1];
		int gx = read_le<int16_t>(p + 6), gy = read_le<int16_t>(p + 8);
		int screen = -1;
		for (size_t s = 0; s < n; ++s)
			if (touch_devs[s].bounds.contains(gx, gy))
				screen = int(s);
		bool down = active && p[2];
		int & on = finger_screen[i];
		if (on >= 0 && (!down || (screen >= 0 && screen != on))) // lifted, or dragged onto another screen
		{
			finger(on, i, false, gx, gy);
			on = -1;
		}
		if (down && on < 0 && screen >= 0)
			on = screen;
		if (down && on >= 0) // off every screen while held: stays on its screen, clamped to the edge
			finger(on, i, true, gx, gy);

		bool right = active && p[3];
		if (right != touch_right[i])
		{
			if (right)
			{
				write(pointer_dev, EV_ABS, ABS_X, std::clamp(gx, 0, current.width - 1));
				write(pointer_dev, EV_ABS, ABS_Y, std::clamp(gy, 0, current.height - 1));
			}
			write(pointer_dev, EV_KEY, BTN_RIGHT, right);
			syn(pointer_dev);
			touch_right[i] = right;
		}
	}
	for (size_t s = 0; s < n; ++s)
	{
		if (!dirty[s])
			continue;
		touch_device & t = touch_devs[s];
		int fingers = int(t.touching[0]) + int(t.touching[1]);
		int primary = t.touching[0] ? 0 : 1;
		if (fingers)
		{
			write(t.dev, EV_ABS, ABS_X, t.x[primary]);
			write(t.dev, EV_ABS, ABS_Y, t.y[primary]);
		}
		if ((fingers > 0) != (fingers_before[s] > 0))
			write(t.dev, EV_KEY, BTN_TOUCH, fingers > 0);
		write(t.dev, EV_KEY, BTN_TOOL_FINGER, fingers == 1);
		write(t.dev, EV_KEY, BTN_TOOL_DOUBLETAP, fingers == 2);
		syn(t.dev);
	}
}

void input_injector::mouse(const uint8_t * fd)
{
	const uint8_t * m = fd + mouse_offset;
	if (!m[0])
		return;
	bool dirty = false;
	frac_x += read_le<float>(m + 8);
	frac_y += read_le<float>(m + 12);
	for (auto [frac, code]: {std::pair{&frac_x, REL_X}, std::pair{&frac_y, REL_Y}})
	{
		int step = int(*frac);
		if (step)
		{
			*frac -= float(step);
			write(kbd_mouse_dev, EV_REL, code, step);
			dirty = true;
		}
	}
	int8_t sx = int8_t(m[16]), sy = int8_t(m[17]);
	struct wheel
	{
		int8_t value;
		int * acc;
		int hi, lo;
	};
	for (wheel w: {wheel{sy, &wheel_acc_v, REL_WHEEL_HI_RES, REL_WHEEL}, wheel{sx, &wheel_acc_h, REL_HWHEEL_HI_RES, REL_HWHEEL}})
	{
		if (!w.value)
			continue;
		int v = w.value * scroll_hires_per_unit;
		write(kbd_mouse_dev, EV_REL, w.hi, v);
		*w.acc += v;
		if (int notches = *w.acc / 120)
		{
			*w.acc -= notches * 120;
			write(kbd_mouse_dev, EV_REL, w.lo, notches);
		}
		dirty = true;
	}
	static constexpr int codes[] = {BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA};
	for (int b = 0; b < 5; ++b)
	{
		bool now = m[1 + b];
		if (now != mouse_buttons[b])
		{
			write(kbd_mouse_dev, EV_KEY, codes[b], now);
			mouse_buttons[b] = now;
			dirty = true;
		}
	}
	if (dirty)
		syn(kbd_mouse_dev);
}

void input_injector::keyboard(const uint8_t * fd)
{
	std::set<int> down, taps;
	for (size_t i = 0; i < key_slots; ++i)
	{
		const uint8_t * slot = fd + keyboard_offset + i * key_slot_size;
		uint16_t sc = read_le<uint16_t>(slot);
		int key = sc ? scancode_to_key(sc) : -1;
		if (key < 0)
			continue;
		if (slot[2] & key_down)
			down.insert(key);
		else if (slot[2] & key_pressed)
			taps.insert(key);
	}
	bool dirty = false;
	for (int key: down)
		if (!keys_down.contains(key))
		{
			write(kbd_mouse_dev, EV_KEY, key, 1);
			dirty = true;
		}
	for (int key: keys_down)
		if (!down.contains(key))
		{
			write(kbd_mouse_dev, EV_KEY, key, 0);
			dirty = true;
		}
	for (int key: taps) // pressed and released within one packet
		if (!down.contains(key) && !keys_down.contains(key))
		{
			write(kbd_mouse_dev, EV_KEY, key, 1);
			syn(kbd_mouse_dev);
			write(kbd_mouse_dev, EV_KEY, key, 0);
			dirty = true;
		}
	keys_down = std::move(down);
	if (dirty)
		syn(kbd_mouse_dev);
}
} // namespace od
