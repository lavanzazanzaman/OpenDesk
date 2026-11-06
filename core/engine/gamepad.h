// Virtual gamepad (uinput) for the headset's gamepad emulation / paired Bluetooth pads, like the Windows streamer's
// ViGEm-based GamepadEmulation. RawGamepadState in FrameData @80 (dcgen --layout), model hint @112.
// Rumble: games upload force-feedback effects to the pad; they are answered here and the current strong/weak
// magnitudes are exposed so the video frames can carry them to the headset (FrameMetadata Left/RightVibration).
#pragma once

#include "bytes.h"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

struct libevdev_uinput;

namespace od
{
enum class gamepad_model : uint8_t
{
	xbox360 = 0,   // Upstream.Interfaces.GamepadModel
	dualshock4 = 1,
};

class virtual_gamepad
{
public:
	using rumble_callback = std::function<void(float strong, float weak)>; // any thread

	virtual_gamepad(gamepad_model, rumble_callback);
	~virtual_gamepad();
	virtual_gamepad(const virtual_gamepad &) = delete;
	virtual_gamepad & operator=(const virtual_gamepad &) = delete;

	gamepad_model model() const { return kind; }
	void apply(const uint8_t * raw_gamepad_state); // 13 bytes at FrameData+80

private:
	void ff_loop();
	void update_rumble();

	gamepad_model kind;
	rumble_callback on_rumble;
	libevdev_uinput * dev = nullptr;
	std::array<int, 16> last{};
	bool first = true;

	struct effect
	{
		uint16_t strong = 0, weak = 0;
		bool playing = false;
	};
	std::mutex ff_mutex;
	std::map<int, effect> effects;
	std::atomic<bool> quit{false};
	std::thread ff_thread;
};

// true when the pad state carries any input (RawGamepadState.IsActive)
bool gamepad_state_active(const uint8_t * raw_gamepad_state);
} // namespace od
