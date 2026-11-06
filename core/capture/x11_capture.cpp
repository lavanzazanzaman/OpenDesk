#include "x11_capture.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <sys/ipc.h>
#include <sys/shm.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>

namespace od
{
namespace
{
constexpr uint32_t drm_xrgb8888 = 0x34325258; // 'XR24'
} // namespace

bool x11_session()
{
	const char * type = std::getenv("XDG_SESSION_TYPE");
	if (type && *type)
		return std::strcmp(type, "x11") == 0;
	const char * wl = std::getenv("WAYLAND_DISPLAY");
	const char * x = std::getenv("DISPLAY");
	return (!wl || !*wl) && x && *x;
}

std::vector<display_info> x11_monitors()
{
	std::vector<display_info> list;
	Display * dpy = XOpenDisplay(nullptr);
	if (!dpy)
		return list;
	int n = 0;
	XRRMonitorInfo * mon = XRRGetMonitors(dpy, DefaultRootWindow(dpy), True, &n);
	for (int i = 0; mon && i < n; ++i)
	{
		display_info d;
		char * name = mon[i].name != None ? XGetAtomName(dpy, mon[i].name) : nullptr;
		d.name = name ? name : std::format("Screen {}", i + 1);
		if (name)
			XFree(name);
		d.description = "X11 monitor";
		d.x = mon[i].x;
		d.y = mon[i].y;
		d.width = d.pixel_width = mon[i].width;
		d.height = d.pixel_height = mon[i].height;
		d.refresh_mhz = 60000;
		if (mon[i].primary)
			list.insert(list.begin(), d);
		else
			list.push_back(d);
	}
	if (mon)
		XRRFreeMonitors(mon);
	if (list.empty()) // no RandR monitors: the whole screen
	{
		display_info d;
		d.name = "Screen 1";
		d.description = "X11 screen";
		d.width = d.pixel_width = DisplayWidth(dpy, DefaultScreen(dpy));
		d.height = d.pixel_height = DisplayHeight(dpy, DefaultScreen(dpy));
		d.refresh_mhz = 60000;
		list.push_back(d);
	}
	XCloseDisplay(dpy);
	return list;
}

x11_capture::x11_capture(int x_, int y_, int w, int h, int max_fps, frame_callback cb, log_callback l) :
        x(x_), y(y_), width(w & ~1), height(h & ~1), fps(std::clamp(max_fps, 1, 240)), on_frame(std::move(cb)), log(std::move(l))
{
	thread = std::thread(&x11_capture::run, this);
}

x11_capture::~x11_capture()
{
	quit = true;
	if (thread.joinable())
		thread.join();
}

void x11_capture::run()
{
	Display * dpy = XOpenDisplay(nullptr);
	if (!dpy || !XShmQueryExtension(dpy))
	{
		if (log)
			log(dpy ? "[capture] X11: no MIT-SHM extension" : "[capture] X11: cannot open the display");
		if (dpy)
			XCloseDisplay(dpy);
		error = true;
		return;
	}
	int fixes_event = 0, fixes_error = 0;
	const bool fixes = XFixesQueryExtension(dpy, &fixes_event, &fixes_error);
	Window root = DefaultRootWindow(dpy);
	XShmSegmentInfo shm{};
	XImage * img = XShmCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), DefaultDepth(dpy, DefaultScreen(dpy)), ZPixmap, nullptr,
	                               &shm, unsigned(width), unsigned(height));
	if (img)
	{
		shm.shmid = shmget(IPC_PRIVATE, size_t(img->bytes_per_line) * img->height, IPC_CREAT | 0600);
		shm.shmaddr = img->data = shm.shmid >= 0 ? static_cast<char *>(shmat(shm.shmid, nullptr, 0)) : nullptr;
		shm.readOnly = False;
	}
	if (!img || !shm.shmaddr || shm.shmaddr == reinterpret_cast<char *>(-1) || !XShmAttach(dpy, &shm) || img->bits_per_pixel != 32)
	{
		if (log)
			log("[capture] X11: shared-memory image setup failed (needs a 24/32-bit TrueColor screen)");
		error = true;
	}
	else
	{
		XSync(dpy, False);
		shmctl(shm.shmid, IPC_RMID, nullptr); // freed once both sides detach
		if (log)
			log(std::format("[capture] X11 screen grab {}x{} at {},{} (MIT-SHM{})", width, height, x, y, fixes ? ", cursor" : ""));
		const auto period = std::chrono::nanoseconds(1000000000LL / fps);
		auto next = std::chrono::steady_clock::now();
		while (!quit)
		{
			if (!XShmGetImage(dpy, root, img, x, y, AllPlanes))
			{
				if (log)
					log("[capture] X11: grab failed (screen layout changed?)");
				error = true;
				break;
			}
			if (fixes) // the pointer, alpha-blended (premultiplied ARGB)
				if (XFixesCursorImage * c = XFixesGetCursorImage(dpy))
				{
					const int cx = c->x - c->xhot - x, cy = c->y - c->yhot - y;
					for (int j = 0; j < c->height; ++j)
					{
						int py = cy + j;
						if (py < 0 || py >= height)
							continue;
						auto * row = reinterpret_cast<uint32_t *>(img->data + size_t(py) * img->bytes_per_line);
						for (int i = 0; i < c->width; ++i)
						{
							int px = cx + i;
							if (px < 0 || px >= width)
								continue;
							uint32_t s = uint32_t(c->pixels[size_t(j) * c->width + i]); // unsigned long per pixel
							uint32_t a = s >> 24;
							if (!a)
								continue;
							uint32_t d = row[px];
							auto mix = [&](int shift) {
								uint32_t sc = (s >> shift) & 255, dc = (d >> shift) & 255;
								return std::min<uint32_t>(255, sc + dc * (255 - a) / 255) << shift;
							};
							row[px] = mix(16) | mix(8) | mix(0);
						}
					}
					XFree(c);
				}
			captured_frame f;
			f.data = reinterpret_cast<const uint8_t *>(img->data);
			f.stride = uint32_t(img->bytes_per_line);
			f.size = size_t(img->bytes_per_line) * height;
			f.drm_format = drm_xrgb8888;
			f.av_sw_format = AV_PIX_FMT_BGR0;
			on_frame(f, width, height);
			next += period;
			auto now = std::chrono::steady_clock::now();
			if (next < now)
				next = now;
			std::this_thread::sleep_until(next);
		}
		XShmDetach(dpy, &shm);
	}
	if (img)
		XDestroyImage(img); // frees nothing of the segment (data was ours)
	if (shm.shmaddr && shm.shmaddr != reinterpret_cast<char *>(-1))
		shmdt(shm.shmaddr);
	XCloseDisplay(dpy);
}
} // namespace od
