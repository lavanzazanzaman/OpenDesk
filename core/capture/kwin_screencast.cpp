#include "kwin_screencast.h"

#include "kde-output-order-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"
#include <wayland-client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <set>
#include <thread>
#include <utility>

namespace od
{
struct kwin_screencast::output
{
	kwin_screencast * owner = nullptr;
	wl_output * wl = nullptr;
	zxdg_output_v1 * xdg = nullptr;
	display_info info;
	int mode_w = 0, mode_h = 0;
};

struct kwin_screencast::stream
{
	zkde_screencast_stream_unstable_v1 * wl = nullptr;
	stream_node node;
	bool created = false, failed = false, closed = false;
	std::string error;
	std::string virtual_output; // for virtual outputs: the output this stream keeps alive
};

namespace
{
constexpr uint32_t pointer_embedded = 2;

void update_pixels(display_info & i, int mode_w, int mode_h)
{
	bool rotated = i.transform & 1; // 90/270 and their flipped variants
	i.pixel_width = rotated ? mode_h : mode_w;
	i.pixel_height = rotated ? mode_w : mode_h;
}

const wl_output_listener output_listener = {
        .geometry = [](void * d, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *,
                       int32_t transform) {
	        auto * o = static_cast<kwin_screencast::output *>(d);
	        o->info.transform = transform;
	        update_pixels(o->info, o->mode_w, o->mode_h);
        },
        .mode = [](void * d, wl_output *, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
	        auto * o = static_cast<kwin_screencast::output *>(d);
	        if (!(flags & WL_OUTPUT_MODE_CURRENT))
		        return;
	        o->mode_w = w;
	        o->mode_h = h;
	        o->info.refresh_mhz = refresh;
	        update_pixels(o->info, w, h);
        },
        .done = [](void *, wl_output *) {},
        .scale = [](void *, wl_output *, int32_t) {},
        .name = [](void * d, wl_output *, const char * name) { static_cast<kwin_screencast::output *>(d)->info.name = name; },
        .description = [](void * d, wl_output *, const char * desc) {
	        static_cast<kwin_screencast::output *>(d)->info.description = desc;
        },
};

const zxdg_output_v1_listener xdg_output_listener = {
        .logical_position = [](void * d, zxdg_output_v1 *, int32_t x, int32_t y) {
	        auto * o = static_cast<kwin_screencast::output *>(d);
	        o->info.x = x;
	        o->info.y = y;
        },
        .logical_size = [](void * d, zxdg_output_v1 *, int32_t w, int32_t h) {
	        auto * o = static_cast<kwin_screencast::output *>(d);
	        o->info.width = w;
	        o->info.height = h;
        },
        .done = [](void *, zxdg_output_v1 *) {},
        .name = [](void *, zxdg_output_v1 *, const char *) {},
        .description = [](void *, zxdg_output_v1 *, const char *) {},
};

const zkde_screencast_stream_unstable_v1_listener stream_listener = {
        .closed = [](void * d, zkde_screencast_stream_unstable_v1 *) { static_cast<kwin_screencast::stream *>(d)->closed = true; },
        .created = [](void * d, zkde_screencast_stream_unstable_v1 *, uint32_t node) {
	        auto * s = static_cast<kwin_screencast::stream *>(d);
	        s->node.node_id = node;
	        s->created = true;
        },
        .failed = [](void * d, zkde_screencast_stream_unstable_v1 *, const char * error) {
	        auto * s = static_cast<kwin_screencast::stream *>(d);
	        s->failed = true;
	        s->error = error ? error : "failed";
        },
        .serial = [](void * d, zkde_screencast_stream_unstable_v1 *, uint32_t hi, uint32_t lo) {
	        static_cast<kwin_screencast::stream *>(d)->node.serial = (uint64_t(hi) << 32) | lo;
        },
};

// the whole list is resent after every change, terminated by "done"
std::vector<std::string> incoming_order;
const kde_output_order_v1_listener order_listener = {
        .output = [](void *, kde_output_order_v1 *, const char * name) { incoming_order.push_back(name); },
        .done = [](void * d, kde_output_order_v1 *) { static_cast<kwin_screencast *>(d)->set_order(std::move(incoming_order)); },
};
} // namespace

std::unique_ptr<kwin_screencast> kwin_screencast::connect(std::string * why)
{
	std::unique_ptr<kwin_screencast> k(new kwin_screencast);
	k->display = wl_display_connect(nullptr);
	if (!k->display)
	{
		if (why)
			*why = "no Wayland session";
		return nullptr;
	}
	static const wl_registry_listener listener = {registry_global, registry_remove};
	k->registry = wl_display_get_registry(k->display);
	wl_registry_add_listener(k->registry, &listener, k.get());
	wl_display_roundtrip(k->display); // globals
	wl_display_roundtrip(k->display); // output properties
	if (!k->screencast)
	{
		if (why)
			*why = "KWin screen-cast protocol not granted (needs the .desktop entry with X-KDE-Wayland-Interfaces)";
		return nullptr;
	}
	k->changed = false;
	return k;
}

kwin_screencast::~kwin_screencast()
{
	for (auto & [h, s]: streams)
		if (s->wl)
			zkde_screencast_stream_unstable_v1_close(s->wl);
	streams.clear();
	for (auto & [n, o]: outputs)
	{
		if (o->xdg)
			zxdg_output_v1_destroy(o->xdg);
		if (o->wl)
			wl_output_release(o->wl);
	}
	outputs.clear();
	if (output_order)
		kde_output_order_v1_destroy(output_order);
	if (screencast)
		zkde_screencast_unstable_v1_destroy(screencast);
	if (xdg_output_manager)
		zxdg_output_manager_v1_destroy(xdg_output_manager);
	if (registry)
		wl_registry_destroy(registry);
	if (display)
	{
		wl_display_flush(display);
		wl_display_disconnect(display);
	}
}

void kwin_screencast::registry_global(void * data, wl_registry * reg, uint32_t name, const char * iface, uint32_t version)
{
	auto * k = static_cast<kwin_screencast *>(data);
	if (!std::strcmp(iface, wl_output_interface.name))
		k->add_output(name, version);
	else if (!std::strcmp(iface, zxdg_output_manager_v1_interface.name))
	{
		k->xdg_output_manager = static_cast<zxdg_output_manager_v1 *>(
		        wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, std::min(version, 3u)));
		for (auto & [n, o]: k->outputs) // outputs announced before the manager
			if (!o->xdg)
			{
				o->xdg = zxdg_output_manager_v1_get_xdg_output(k->xdg_output_manager, o->wl);
				zxdg_output_v1_add_listener(o->xdg, &xdg_output_listener, o.get());
			}
	}
	else if (!std::strcmp(iface, zkde_screencast_unstable_v1_interface.name))
		k->screencast = static_cast<zkde_screencast_unstable_v1 *>(
		        wl_registry_bind(reg, name, &zkde_screencast_unstable_v1_interface,
		                         std::min<uint32_t>(version, zkde_screencast_unstable_v1_interface.version)));
	else if (!std::strcmp(iface, kde_output_order_v1_interface.name))
	{
		k->output_order = static_cast<kde_output_order_v1 *>(wl_registry_bind(reg, name, &kde_output_order_v1_interface, 1));
		kde_output_order_v1_add_listener(k->output_order, &order_listener, k);
	}
}

void kwin_screencast::add_output(uint32_t global, uint32_t version)
{
	if (version < 4) // need the name event
		return;
	auto o = std::make_unique<output>();
	o->owner = this;
	o->wl = static_cast<wl_output *>(wl_registry_bind(registry, global, &wl_output_interface, 4));
	wl_output_add_listener(o->wl, &output_listener, o.get());
	if (xdg_output_manager)
	{
		o->xdg = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, o->wl);
		zxdg_output_v1_add_listener(o->xdg, &xdg_output_listener, o.get());
	}
	outputs[global] = std::move(o);
	changed = true;
}

void kwin_screencast::registry_remove(void * data, wl_registry *, uint32_t name)
{
	auto * k = static_cast<kwin_screencast *>(data);
	auto it = k->outputs.find(name);
	if (it == k->outputs.end())
		return;
	if (it->second->xdg)
		zxdg_output_v1_destroy(it->second->xdg);
	wl_output_release(it->second->wl);
	k->outputs.erase(it);
	k->changed = true;
}

int kwin_screencast::fd() const
{
	return wl_display_get_fd(display);
}

void kwin_screencast::pump()
{
	while (wl_display_prepare_read(display) != 0)
		wl_display_dispatch_pending(display);
	wl_display_flush(display);
	pollfd p{wl_display_get_fd(display), POLLIN, 0};
	if (poll(&p, 1, 0) > 0)
		wl_display_read_events(display);
	else
		wl_display_cancel_read(display);
	wl_display_dispatch_pending(display);
}

std::vector<display_info> kwin_screencast::displays()
{
	wl_display_roundtrip(display);
	std::vector<display_info> list;
	for (auto & [n, o]: outputs)
		if (!o->info.name.empty() && o->info.width > 0)
		{
			display_info i = o->info;
			i.is_virtual = std::find(virtual_names.begin(), virtual_names.end(), i.name) != virtual_names.end();
			list.push_back(i);
		}
	auto rank = [&](const display_info & i) {
		auto it = std::find(order.begin(), order.end(), i.name);
		return it == order.end() ? order.size() : size_t(it - order.begin());
	};
	std::stable_sort(list.begin(), list.end(), [&](const display_info & a, const display_info & b) {
		if (a.is_virtual != b.is_virtual) // ours last, in creation order
			return !a.is_virtual;
		if (a.is_virtual)
			return std::find(virtual_names.begin(), virtual_names.end(), a.name) <
			       std::find(virtual_names.begin(), virtual_names.end(), b.name);
		if (rank(a) != rank(b))
			return rank(a) < rank(b);
		return std::pair(a.x, a.y) < std::pair(b.x, b.y);
	});
	return list;
}

void kwin_screencast::set_order(std::vector<std::string> && names)
{
	if (names != order)
		changed = true;
	order = std::move(names);
	incoming_order.clear();
}

bool kwin_screencast::take_changed()
{
	pump();
	return std::exchange(changed, false);
}

bool kwin_screencast::wait(stream & s, std::string * error)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!s.created && !s.failed && !s.closed)
	{
		if (wl_display_roundtrip(display) < 0)
		{
			s.failed = true;
			s.error = "lost the connection to the compositor";
			break;
		}
		if (s.created || s.failed || s.closed)
			break;
		if (std::chrono::steady_clock::now() > deadline)
		{
			s.failed = true;
			s.error = "the compositor didn't create the stream in time";
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (s.created)
		return true;
	if (error)
		*error = s.closed ? "the compositor closed the stream" : s.error;
	return false;
}

std::optional<int> kwin_screencast::stream_output(const std::string & name, stream_node * node, std::string * error)
{
	wl_output * target = nullptr;
	for (auto & [n, o]: outputs)
		if (o->info.name == name)
			target = o->wl;
	if (!target)
	{
		if (error)
			*error = "no output named " + name;
		return std::nullopt;
	}
	auto s = std::make_unique<stream>();
	s->wl = zkde_screencast_unstable_v1_stream_output(screencast, target, pointer_embedded);
	zkde_screencast_stream_unstable_v1_add_listener(s->wl, &stream_listener, s.get());
	if (!wait(*s, error))
	{
		zkde_screencast_stream_unstable_v1_close(s->wl);
		wl_display_flush(display);
		return std::nullopt;
	}
	if (node)
		*node = s->node;
	int h = next_handle++;
	streams[h] = std::move(s);
	return h;
}

std::optional<int> kwin_screencast::stream_virtual_output(const std::string & name, const std::string & description, int width,
                                                          int height, double scale, stream_node * node,
                                                          std::string * output_name, std::string * error)
{
	std::set<std::string> before;
	for (auto & [n, o]: outputs)
		before.insert(o->info.name);
	auto s = std::make_unique<stream>();
	uint32_t version = zkde_screencast_unstable_v1_get_version(screencast);
	if (version >= 4)
		s->wl = zkde_screencast_unstable_v1_stream_virtual_output_with_description(
		        screencast, name.c_str(), description.c_str(), width, height, wl_fixed_from_double(scale), pointer_embedded);
	else if (version >= 2)
		s->wl = zkde_screencast_unstable_v1_stream_virtual_output(screencast, name.c_str(), width, height,
		                                                          wl_fixed_from_double(scale), pointer_embedded);
	else
	{
		if (error)
			*error = "KWin is too old for virtual outputs";
		return std::nullopt;
	}
	zkde_screencast_stream_unstable_v1_add_listener(s->wl, &stream_listener, s.get());
	if (!wait(*s, error))
	{
		zkde_screencast_stream_unstable_v1_close(s->wl);
		wl_display_flush(display);
		return std::nullopt;
	}
	// the new wl_output global (+ its name/geometry) follows the stream
	for (int i = 0; i < 20 && s->virtual_output.empty(); ++i)
	{
		wl_display_roundtrip(display);
		for (auto & [n, o]: outputs)
			if (!o->info.name.empty() && o->info.width > 0 && !before.contains(o->info.name))
				s->virtual_output = o->info.name;
		if (s->virtual_output.empty())
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (!s->virtual_output.empty())
		virtual_names.push_back(s->virtual_output);
	if (node)
		*node = s->node;
	if (output_name)
		*output_name = s->virtual_output;
	int h = next_handle++;
	streams[h] = std::move(s);
	changed = true;
	return h;
}

void kwin_screencast::close(int handle)
{
	auto it = streams.find(handle);
	if (it == streams.end())
		return;
	if (!it->second->virtual_output.empty())
		std::erase(virtual_names, it->second->virtual_output);
	zkde_screencast_stream_unstable_v1_close(it->second->wl);
	streams.erase(it);
	wl_display_flush(display);
}
} // namespace od
