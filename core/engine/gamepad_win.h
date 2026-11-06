// Windows virtual gamepad. Uses ViGEmBus (ViGEmClient.dll) when it is installed; otherwise the
// gamepad is silently unavailable. The wire state is the same as the Linux backend.
#pragma once

#include "bytes.h"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace od
{
enum class gamepad_model : uint8_t
{
	xbox360 = 0,
	dualshock4 = 1,
};

class virtual_gamepad
{
public:
	using rumble_callback = std::function<void(float strong, float weak)>;

	virtual_gamepad(gamepad_model, rumble_callback);
	~virtual_gamepad();
	virtual_gamepad(const virtual_gamepad &) = delete;
	virtual_gamepad & operator=(const virtual_gamepad &) = delete;

	gamepad_model model() const { return kind; }
	void apply(const uint8_t * raw_gamepad_state);

private:
	gamepad_model kind;
	rumble_callback on_rumble;
	std::array<int, 16> last{};
	bool first = true;
	struct impl;
	std::unique_ptr<impl> state;
};

bool gamepad_state_active(const uint8_t * raw_gamepad_state);
} // namespace od
