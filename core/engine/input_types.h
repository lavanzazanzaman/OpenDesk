// Shared input geometry types (platform-neutral).
#pragma once

#include "video.h" // rect

#include <string>
#include <vector>

namespace od
{
// Where the streamed screens are. Laser pointers arrive in desktop coordinates (the VideoFormat Bounds space).
struct input_screen
{
	rect bounds;
	std::string output; // Linux: compositor output name ("DP-1"); Windows: monitor device name; empty = unknown
	bool operator==(const input_screen &) const = default;
};
struct input_layout
{
	int width = 0, height = 0; // whole desktop, same units as the bounds
	std::vector<input_screen> screens;
	bool operator==(const input_layout &) const = default;
};
} // namespace od
