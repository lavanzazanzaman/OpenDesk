// KWin's own screen-cast protocol (zkde_screencast_unstable_v1), the one xdg-desktop-portal-kde and krdp use.
// Unlike the portal it needs no dialog, streams every output separately and can create *virtual outputs*: extra
// monitors that exist only in the headset (the Windows streamer's virtual monitor driver; also keeps a desktop to
// stream while the physical screen is off).
// KWin offers the global only to programs whose .desktop entry lists it:
//   X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1      (installed by the setup check)
// Plain Wayland client with its own connection; not thread-safe, use from one thread and call pump() when fd()
// becomes readable.
#pragma once

#include "display_info.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_output;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;
struct zkde_screencast_unstable_v1;
struct zkde_screencast_stream_unstable_v1;
struct kde_output_order_v1;

namespace od
{
struct stream_node
{
	uint32_t node_id = 0;
	uint64_t serial = 0; // PipeWire object serial (protocol v6+); preferred over the node id
};

class kwin_screencast
{
public:
	// nullptr when the compositor is not KWin or doesn't grant us the protocol (reason in *why)
	static std::unique_ptr<kwin_screencast> connect(std::string * why);
	~kwin_screencast();
	kwin_screencast(const kwin_screencast &) = delete;
	kwin_screencast & operator=(const kwin_screencast &) = delete;

	int fd() const;
	void pump(); // dispatch whatever arrived (non-blocking)
	// outputs in the desktop's priority order (primary first); virtual outputs we created are flagged
	std::vector<display_info> displays();
	// true (once) when outputs were added/removed/changed since the last call
	bool take_changed();

	// Streams an existing output. Returns a handle for close(); blocks until KWin created the PipeWire node.
	std::optional<int> stream_output(const std::string & output_name, stream_node * node, std::string * error);
	// Creates a virtual output (logical size, scale) and streams it; the output lives as long as the stream.
	std::optional<int> stream_virtual_output(const std::string & name, const std::string & description, int width,
	                                         int height, double scale, stream_node * node, std::string * output_name,
	                                         std::string * error);
	void close(int handle);

	struct output; // internal
	struct stream;
	void set_order(std::vector<std::string> && names);

private:
	kwin_screencast() = default;
	static void registry_global(void *, wl_registry *, uint32_t name, const char * iface, uint32_t version);
	static void registry_remove(void *, wl_registry *, uint32_t name);
	void add_output(uint32_t global, uint32_t version);
	bool wait(stream &, std::string * error);

	wl_display * display = nullptr;
	wl_registry * registry = nullptr;
	zxdg_output_manager_v1 * xdg_output_manager = nullptr;
	zkde_screencast_unstable_v1 * screencast = nullptr;
	kde_output_order_v1 * output_order = nullptr;
	std::vector<std::string> order;
	std::map<uint32_t, std::unique_ptr<output>> outputs; // by registry name
	std::map<int, std::unique_ptr<stream>> streams;
	std::vector<std::string> virtual_names;
	int next_handle = 1;
	bool changed = false;
};
} // namespace od
