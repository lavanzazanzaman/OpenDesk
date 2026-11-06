// Headset FrameData (Upstream.Mobile.FrameData, 4992 bytes, little-endian) accessors for VR tracking.
// Offsets: tools/dcgen --layout (work/dumps/FrameData.layout.txt). Header-only: used by the streamer and libovr-opendesk.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace od::frame_data
{
namespace offset
{
constexpr size_t frame_index = 0, floor_height = 8;
constexpr size_t buttons = 94, touches = 96; // ControllerState
constexpr size_t left_trigger = 98, right_trigger = 99, left_grip = 100, right_grip = 101;
constexpr size_t left_thumb = 102, right_thumb = 106; // i16 x, y
constexpr size_t head = 120;                          // orientation, position, velocity, angular velocity, flags @172
constexpr size_t head_tracked = 172, head_mounted = 173;
constexpr size_t controller[2] = {176, 244};          // orientation, position, velocity, acceleration, ang. vel, @+64 flags
constexpr size_t hands_active = 312;                  // u8 left, u8 right
constexpr size_t hand_joints[2] = {316, 1772};        // 1456 bytes each
constexpr size_t hand_aim[2] = {3232, 3288};
constexpr size_t eyes_valid = 4632, eye_pose[2] = {4636, 4664};
} // namespace offset

// Upstream.Mobile.ControllerButtonFlags / ControllerTouchFlags
enum button : uint16_t
{
	button_a = 1, button_b = 2, button_x = 4, button_y = 8, button_rthumb = 0x10, button_rtrigger = 0x20, button_rgrip = 0x40,
	button_lthumb = 0x80, button_ltrigger = 0x100, button_lgrip = 0x200, button_enter = 0x400, button_back = 0x800,
};
enum touch : uint16_t
{
	touch_a = 1, touch_b = 2, touch_x = 4, touch_y = 8, touch_rthumb = 0x10, touch_rtrigger = 0x20, touch_rthumbrest = 0x40,
	touch_lthumb = 0x80, touch_ltrigger = 0x100, touch_lthumbrest = 0x200, touch_rindex_pointing = 0x400,
	touch_rthumb_up = 0x800, touch_lindex_pointing = 0x1000, touch_lthumb_up = 0x2000,
};

struct pose
{
	std::array<float, 4> orientation{0, 0, 0, 1};
	std::array<float, 3> position{};
	std::array<float, 3> velocity{}, angular_velocity{};
	bool tracked = false;
};

template <typename T>
inline T get(const uint8_t * fd, size_t off)
{
	T v;
	std::memcpy(&v, fd + off, sizeof(T));
	return v;
}

inline pose head(const uint8_t * fd)
{
	pose p;
	std::memcpy(p.orientation.data(), fd + offset::head, 16);
	std::memcpy(p.position.data(), fd + offset::head + 16, 12);
	std::memcpy(p.velocity.data(), fd + offset::head + 28, 12);
	std::memcpy(p.angular_velocity.data(), fd + offset::head + 40, 12);
	p.tracked = fd[offset::head_tracked];
	return p;
}

inline pose controller(const uint8_t * fd, int hand)
{
	size_t o = offset::controller[hand];
	pose p;
	std::memcpy(p.orientation.data(), fd + o, 16);
	std::memcpy(p.position.data(), fd + o + 16, 12);
	std::memcpy(p.velocity.data(), fd + o + 28, 12);
	std::memcpy(p.angular_velocity.data(), fd + o + 52, 12);
	p.tracked = fd[o + 64] & 1;
	return p;
}

inline float trigger(const uint8_t * fd, int hand) { return fd[hand ? offset::right_trigger : offset::left_trigger] / 255.f; }
inline float grip(const uint8_t * fd, int hand) { return fd[hand ? offset::right_grip : offset::left_grip] / 255.f; }
inline std::array<float, 2> thumbstick(const uint8_t * fd, int hand)
{
	size_t o = hand ? offset::right_thumb : offset::left_thumb;
	return {get<int16_t>(fd, o) / 32767.f, get<int16_t>(fd, o + 2) / 32767.f};
}
inline uint16_t buttons(const uint8_t * fd) { return get<uint16_t>(fd, offset::buttons); }
inline uint16_t touches(const uint8_t * fd) { return get<uint16_t>(fd, offset::touches); }
} // namespace od::frame_data
