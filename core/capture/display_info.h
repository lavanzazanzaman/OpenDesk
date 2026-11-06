// One streamable display (platform-neutral).
#pragma once

#include <string>

namespace od
{
struct display_info
{
	std::string name;        // connector ("DP-1") or Windows device name ("\\\\.\\DISPLAY1")
	std::string description;
	int x = 0, y = 0, width = 0, height = 0; // desktop coordinates
	int pixel_width = 0, pixel_height = 0;   // current mode, rotated like the display
	int transform = 0;                       // Linux: wl_output transform; Windows: 0 or 1/2/3
	int refresh_mhz = 0;                     // current mode
	bool is_virtual = false;                 // created by us (Linux KWin virtual output)
	bool primary = false;
};
} // namespace od
