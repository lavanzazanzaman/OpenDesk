// FrameData input -> Windows SendInput / touch injection.
// Same wire layout as the Linux backend (core/engine/input.cpp).
#pragma once

#include "bytes.h"
#include "input_types.h"

#include <array>
#include <set>
#include <string>
#include <vector>

namespace od
{
class input_injector
{
public:
	input_injector();
	~input_injector();
	input_injector(const input_injector &) = delete;
	input_injector & operator=(const input_injector &) = delete;

	void apply(const bytes & frame_data);
	void set_touch_mode(bool on);
	bool set_layout(const input_layout &);
	const input_layout & layout() const { return current; }
	std::vector<std::string> touch_devnodes() const { return {}; } // Windows: no device nodes

private:
	void pointers(const uint8_t *);
	void touch_pointers(const uint8_t *);
	void release_touches();
	void mouse(const uint8_t *);
	void keyboard(const uint8_t *);

	void key_event(uint16_t scancode, bool down);
	void absolute_move(int x, int y);
	void mouse_button(int button, bool down);
	void desktop_rect(int & x, int & y, int & w, int & h) const;

	input_layout current;
	bool touch_mode = true;
	bool touch_ready = false;
	std::array<bool, 2> touch_down{};
	std::array<int, 2> touch_x{}, touch_y{};
	std::array<int, 2> finger_screen{-1, -1};
	std::array<bool, 2> touch_right{};
	std::array<int, 2> last_x{-1, -1}, last_y{-1, -1};
	std::array<bool, 3> pointer_buttons{};
	std::array<bool, 5> mouse_buttons{};
	std::set<uint16_t> keys_down;
	float frac_x = 0, frac_y = 0;
	int wheel_acc_v = 0, wheel_acc_h = 0;
};
} // namespace od
