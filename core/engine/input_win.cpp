#include "input_win.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace od
{
namespace
{
constexpr size_t input_end = 118;
constexpr size_t left_pointer = 12, right_pointer = 22;
constexpr size_t keyboard_offset = 32, key_slots = 8, key_slot_size = 3;
constexpr size_t mouse_offset = 60;
constexpr uint8_t key_down = 1, key_pressed = 2;
constexpr int wheel_per_unit = WHEEL_DELTA; // one headset notch = one wheel detent

int16_t read_i16(const uint8_t * p) { return read_le<int16_t>(p); }

// PC set-1 scancodes, as the headset sends them. 0xE0xx keys keep the E0 prefix.
bool extended(uint16_t sc) { return (sc & 0xFF00) == 0xE000; }
uint16_t base_scancode(uint16_t sc) { return extended(sc) ? uint16_t(sc & 0xFF) : sc; }

bool valid_scancode(uint16_t sc)
{
	if (!sc)
		return false;
	if (extended(sc))
		return true;
	return sc <= 0x7F; // set 1 make codes
}
} // namespace

input_injector::input_injector()
{
	// Touch injection needs a process-wide initialization. It fails silently on systems where it is disabled.
	touch_ready = InitializeTouchInjection(10, TOUCH_FEEDBACK_NONE) != FALSE;
}

input_injector::~input_injector()
{
	release_touches();
	for (uint16_t sc: keys_down)
		key_event(sc, false);
	keys_down.clear();
	for (int b = 0; b < 3; ++b)
		if (pointer_buttons[b])
			mouse_button(b, false);
	for (int b = 0; b < 5; ++b)
		if (mouse_buttons[b])
			mouse_button(b + 3, false);
}

void input_injector::desktop_rect(int & x, int & y, int & w, int & h) const
{
	x = GetSystemMetrics(SM_XVIRTUALSCREEN);
	y = GetSystemMetrics(SM_YVIRTUALSCREEN);
	w = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 1);
	h = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 1);
}

bool input_injector::set_layout(const input_layout & l)
{
	if (l == current || l.width <= 0 || l.height <= 0 || l.screens.empty())
		return false;
	release_touches();
	current = l;
	last_x = last_y = {-1, -1};
	return true;
}

void input_injector::set_touch_mode(bool on)
{
	if (on == touch_mode)
		return;
	release_touches();
	if (touch_mode)
		for (int b = 0; b < 3; ++b)
			if (pointer_buttons[b])
			{
				mouse_button(b, false);
				pointer_buttons[b] = false;
			}
	touch_mode = on;
}

void input_injector::release_touches()
{
	if (!touch_ready)
	{
		touch_down = {};
		finger_screen = {-1, -1};
		return;
	}
	for (int i = 0; i < 2; ++i)
		if (touch_down[i] && finger_screen[i] >= 0)
		{
			POINTER_TOUCH_INFO info{};
			info.pointerInfo.pointerType = PT_TOUCH;
			info.pointerInfo.pointerId = DWORD(i);
			info.pointerInfo.ptPixelLocation.x = touch_x[i];
			info.pointerInfo.ptPixelLocation.y = touch_y[i];
			info.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_INRANGE;
			info.touchFlags = TOUCH_FLAG_NONE;
			info.touchMask = TOUCH_MASK_NONE;
			InjectTouchInput(1, &info);
		}
	touch_down = {};
	finger_screen = {-1, -1};
}

void input_injector::key_event(uint16_t sc, bool down)
{
	if (!valid_scancode(sc))
		return;
	INPUT in{};
	in.type = INPUT_KEYBOARD;
	in.ki.wScan = base_scancode(sc);
	in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (extended(sc) ? KEYEVENTF_EXTENDEDKEY : 0);
	SendInput(1, &in, sizeof(in));
}

void input_injector::absolute_move(int x, int y)
{
	int vx, vy, vw, vh;
	desktop_rect(vx, vy, vw, vh);
	INPUT in{};
	in.type = INPUT_MOUSE;
	in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
	in.mi.dx = std::clamp(LONG((int64_t(x - vx) * 65535) / std::max(vw - 1, 1)), 0L, 65535L);
	in.mi.dy = std::clamp(LONG((int64_t(y - vy) * 65535) / std::max(vh - 1, 1)), 0L, 65535L);
	SendInput(1, &in, sizeof(in));
}

void input_injector::mouse_button(int button, bool down)
{
	// 0 left, 1 right, 2 middle, 3 X1, 4 X2
	INPUT in{};
	in.type = INPUT_MOUSE;
	switch (button)
	{
		case 0: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
		case 1: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
		case 2: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
		case 3:
		case 4:
			in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
			in.mi.mouseData = button == 3 ? XBUTTON1 : XBUTTON2;
			break;
		default: return;
	}
	SendInput(1, &in, sizeof(in));
}

void input_injector::apply(const bytes & fd)
{
	if (fd.size() < input_end)
		return;
	if (touch_mode)
		touch_pointers(fd.data());
	else
		pointers(fd.data());
	mouse(fd.data());
	keyboard(fd.data());
}

void input_injector::pointers(const uint8_t * fd)
{
	std::array<bool, 3> buttons{};
	bool moved = false;
	int i = 0;
	const int deadzone = std::max(4, current.width / 400);
	for (size_t off: {left_pointer, right_pointer})
	{
		const uint8_t * p = fd + off;
		const bool active = p[0], overlay = p[1];
		if (!active || overlay)
		{
			last_x[i] = last_y[i] = -1;
			++i;
			continue;
		}
		const int x = std::clamp<int>(read_i16(p + 6), 0, current.width - 1);
		const int y = std::clamp<int>(read_i16(p + 8), 0, current.height - 1);
		bool clicking = false, holding = false;
		for (int b = 0; b < 3; ++b)
		{
			clicking |= bool(p[2 + b]) != pointer_buttons[b];
			holding |= bool(p[2 + b]);
		}
		const bool deliberate = last_x[i] < 0 || holding || std::abs(x - last_x[i]) > deadzone || std::abs(y - last_y[i]) > deadzone;
		if ((deliberate || clicking) && (x != last_x[i] || y != last_y[i]))
		{
			last_x[i] = x;
			last_y[i] = y;
			absolute_move(x, y);
			moved = true;
		}
		for (int b = 0; b < 3; ++b)
			buttons[b] = buttons[b] || p[2 + b];
		++i;
	}
	for (int b = 0; b < 3; ++b)
		if (buttons[b] != pointer_buttons[b])
		{
			mouse_button(b, buttons[b]);
			pointer_buttons[b] = buttons[b];
			moved = true;
		}
	(void)moved;
}

void input_injector::touch_pointers(const uint8_t * fd)
{
	// Touch injection gives each controller one contact. The first button is the finger, the second is a
	// right-click through the absolute pointer (as on the Linux backend).
	for (int i = 0; i < 2; ++i)
	{
		const uint8_t * p = fd + (i == 0 ? left_pointer : right_pointer);
		const bool active = p[0] && !p[1];
		const int gx = read_i16(p + 6), gy = read_i16(p + 8);
		int screen = -1;
		for (size_t s = 0; s < current.screens.size(); ++s)
			if (current.screens[s].bounds.contains(gx, gy))
				screen = int(s);

		const bool down = active && p[2];
		int & on = finger_screen[i];
		if (on >= 0 && (!down || (screen >= 0 && screen != on)))
		{
			if (touch_ready && touch_down[i])
			{
				POINTER_TOUCH_INFO info{};
				info.pointerInfo.pointerType = PT_TOUCH;
				info.pointerInfo.pointerId = DWORD(i);
				info.pointerInfo.ptPixelLocation.x = touch_x[i];
				info.pointerInfo.ptPixelLocation.y = touch_y[i];
				info.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_INRANGE;
				info.touchFlags = TOUCH_FLAG_NONE;
				info.touchMask = TOUCH_MASK_NONE;
				InjectTouchInput(1, &info);
				touch_down[i] = false;
			}
			on = -1;
		}
		if (down && on < 0 && screen >= 0)
			on = screen;
		if (down && on >= 0 && touch_ready)
		{
			touch_x[i] = std::clamp(gx, 0, current.width - 1);
			touch_y[i] = std::clamp(gy, 0, current.height - 1);
			POINTER_TOUCH_INFO info{};
			info.pointerInfo.pointerType = PT_TOUCH;
			info.pointerInfo.pointerId = DWORD(i);
			info.pointerInfo.ptPixelLocation.x = touch_x[i];
			info.pointerInfo.ptPixelLocation.y = touch_y[i];
			info.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
			if (touch_down[i])
				info.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
			info.touchFlags = TOUCH_FLAG_NONE;
			info.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION;
			info.orientation = 90;
			info.rcContact.left = info.pointerInfo.ptPixelLocation.x - 4;
			info.rcContact.top = info.pointerInfo.ptPixelLocation.y - 4;
			info.rcContact.right = info.pointerInfo.ptPixelLocation.x + 4;
			info.rcContact.bottom = info.pointerInfo.ptPixelLocation.y + 4;
			InjectTouchInput(1, &info);
			touch_down[i] = true;
		}

		const bool right = active && p[3];
		if (right != touch_right[i])
		{
			if (right)
				absolute_move(std::clamp(gx, 0, current.width - 1), std::clamp(gy, 0, current.height - 1));
			mouse_button(1, right);
			touch_right[i] = right;
		}
	}
}

void input_injector::mouse(const uint8_t * fd)
{
	const uint8_t * m = fd + mouse_offset;
	if (!m[0])
		return;
	frac_x += read_le<float>(m + 8);
	frac_y += read_le<float>(m + 12);
	const int dx = int(frac_x), dy = int(frac_y);
	frac_x -= float(dx);
	frac_y -= float(dy);
	if (dx || dy)
	{
		INPUT in{};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = MOUSEEVENTF_MOVE;
		in.mi.dx = dx;
		in.mi.dy = dy;
		SendInput(1, &in, sizeof(in));
	}
	const int8_t sx = int8_t(m[16]), sy = int8_t(m[17]);
	if (sy)
	{
		wheel_acc_v += sy * wheel_per_unit;
		INPUT in{};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = MOUSEEVENTF_WHEEL;
		in.mi.mouseData = DWORD(wheel_acc_v);
		wheel_acc_v = 0;
		SendInput(1, &in, sizeof(in));
	}
	if (sx)
	{
		wheel_acc_h += sx * wheel_per_unit;
		INPUT in{};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
		in.mi.mouseData = DWORD(wheel_acc_h);
		wheel_acc_h = 0;
		SendInput(1, &in, sizeof(in));
	}
	// FrameData mouse buttons: left, middle, right, side, extra (same order as the Linux backend)
	static constexpr int map[5] = {0, 2, 1, 3, 4};
	for (int b = 0; b < 5; ++b)
	{
		const bool now = m[1 + b];
		if (now != mouse_buttons[b])
		{
			mouse_button(map[b], now);
			mouse_buttons[b] = now;
		}
	}
}

void input_injector::keyboard(const uint8_t * fd)
{
	std::set<uint16_t> down, taps;
	for (size_t i = 0; i < key_slots; ++i)
	{
		const uint8_t * slot = fd + keyboard_offset + i * key_slot_size;
		const uint16_t sc = read_le<uint16_t>(slot);
		if (!valid_scancode(sc))
			continue;
		if (slot[2] & key_down)
			down.insert(sc);
		else if (slot[2] & key_pressed)
			taps.insert(sc);
	}
	for (uint16_t sc: down)
		if (!keys_down.contains(sc))
			key_event(sc, true);
	for (uint16_t sc: keys_down)
		if (!down.contains(sc))
			key_event(sc, false);
	for (uint16_t sc: taps)
		if (!down.contains(sc) && !keys_down.contains(sc))
		{
			key_event(sc, true);
			key_event(sc, false);
		}
	keys_down = std::move(down);
}
} // namespace od
