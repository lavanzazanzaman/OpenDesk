// FrameData input -> Linux uinput (same layout as engine/vdinput.py).
// Offsets: dcgen --layout Upstream.Mobile.FrameData (work/dumps/FrameData.layout.txt).
#pragma once

#include "bytes.h"
#include "input_types.h"

#include <array>
#include <set>
#include <string>
#include <vector>

struct libevdev_uinput;

namespace od
{

class input_injector
{
public:
	input_injector(); // keyboard + relative mouse right away; laser pointers once a layout is set
	~input_injector();
	input_injector(const input_injector &) = delete;
	input_injector & operator=(const input_injector &) = delete;

	void apply(const bytes & frame_data);
	// upstream "Use touch input": laser pointers become touchscreen fingers (no PC cursor movement, the headset draws
	// its own laser dot); off = they drive the absolute mouse pointer
	void set_touch_mode(bool on);
	// (re)creates the pointer and touch devices when the layout changed; true if it did
	bool set_layout(const input_layout &);
	const input_layout & layout() const { return current; }
	std::vector<std::string> touch_devnodes() const; // per screen: /dev/input/eventN

private:
	void pointers(const uint8_t *);
	void touch_pointers(const uint8_t *);
	void release_touches();
	void mouse(const uint8_t *);
	void keyboard(const uint8_t *);

	void destroy_pointer_devices();

	input_layout current;
	libevdev_uinput * pointer_dev = nullptr;   // absolute: "OpenDesk Pointer"
	struct touch_device                        // multitouch: "OpenDesk Touch [n]" (one slot per controller)
	{
		libevdev_uinput * dev = nullptr;
		rect bounds;
		std::array<bool, 2> touching{};
		std::array<int, 2> x{}, y{};
	};
	std::vector<touch_device> touch_devs;
	std::array<int, 2> finger_screen{-1, -1}; // touch device each controller's finger is down on
	bool touch_mode = true;
	std::array<bool, 2> touch_right{};
	int next_tracking_id = 1;
	libevdev_uinput * kbd_mouse_dev = nullptr; // keys + relative mouse
	std::array<int, 2> last_x{-1, -1}, last_y{-1, -1};
	std::array<bool, 3> pointer_buttons{};
	std::array<bool, 5> mouse_buttons{};
	float frac_x = 0, frac_y = 0;
	int wheel_acc_v = 0, wheel_acc_h = 0;
	std::set<int> keys_down;
};
} // namespace od
