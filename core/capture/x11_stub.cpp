// Built without the X11 development files: no X11 capture (Wayland only)
#include "x11_capture.h"

namespace od
{
bool x11_session() { return false; }
std::vector<display_info> x11_monitors() { return {}; }
x11_capture::x11_capture(int, int, int, int, int, frame_callback, log_callback) { error = true; }
x11_capture::~x11_capture() = default;
void x11_capture::run() {}
} // namespace od
