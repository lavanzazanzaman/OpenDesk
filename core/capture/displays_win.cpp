#include "displays_win.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace od
{
namespace
{
std::string utf8(const wchar_t * w)
{
	if (!w)
		return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 1)
		return {};
	std::string s(size_t(n - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

BOOL CALLBACK monitor_proc(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
	auto * out = reinterpret_cast<std::vector<display_info> *>(data);
	MONITORINFOEXW info{};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(monitor, &info))
		return TRUE;
	display_info d;
	d.name = utf8(info.szDevice);
	d.description = d.name;
	d.x = info.rcMonitor.left;
	d.y = info.rcMonitor.top;
	d.width = info.rcMonitor.right - info.rcMonitor.left;
	d.height = info.rcMonitor.bottom - info.rcMonitor.top;
	d.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
	DEVMODEW mode{};
	mode.dmSize = sizeof(mode);
	if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
	{
		d.refresh_mhz = int(mode.dmDisplayFrequency) * 1000;
		const bool rotated = mode.dmDisplayOrientation == DMDO_90 || mode.dmDisplayOrientation == DMDO_270;
		d.pixel_width = rotated ? int(mode.dmPelsHeight) : int(mode.dmPelsWidth);
		d.pixel_height = rotated ? int(mode.dmPelsWidth) : int(mode.dmPelsHeight);
		d.transform = mode.dmDisplayOrientation == DMDO_90 ? 1 : mode.dmDisplayOrientation == DMDO_180 ? 2
		              : mode.dmDisplayOrientation == DMDO_270 ? 3 : 0;
	}
	else
	{
		d.pixel_width = d.width;
		d.pixel_height = d.height;
	}
	// Friendly description from the display device, when available.
	DISPLAY_DEVICEW device{};
	device.cb = sizeof(device);
	if (EnumDisplayDevicesW(info.szDevice, 0, &device, 0))
		d.description = utf8(device.DeviceString);
	out->push_back(std::move(d));
	return TRUE;
}
} // namespace

display_manager::display_manager(QObject * parent) :
        QObject(parent)
{
}

display_manager::~display_manager() = default;

void display_manager::start(const QString &, std::function<void(QString)> done)
{
	started = true;
	emit displays_changed();
	if (done)
		done({});
}

std::vector<display_info> display_manager::displays()
{
	std::vector<display_info> out;
	EnumDisplayMonitors(nullptr, nullptr, monitor_proc, reinterpret_cast<LPARAM>(&out));
	// Stable order: primary first, then left-to-right, top-to-bottom.
	std::stable_sort(out.begin(), out.end(), [](const display_info & a, const display_info & b) {
		if (a.primary != b.primary)
			return a.primary;
		if (a.x != b.x)
			return a.x < b.x;
		return a.y < b.y;
	});
	return out;
}

display_manager::virtual_mode display_manager::main_monitor_mode()
{
	for (const auto & d: displays())
		if (d.primary)
		{
			virtual_mode m;
			m.width = d.pixel_width;
			m.height = d.pixel_height;
			m.refresh_mhz = d.refresh_mhz ? d.refresh_mhz : 60000;
			return m;
		}
	return {};
}

std::optional<std::string> display_manager::add_virtual(virtual_mode, QString * error)
{
	if (error)
		*error = "virtual displays are not implemented on Windows yet";
	return std::nullopt;
}

bool display_manager::set_portrait(const display_info & d, bool portrait, QString * error)
{
	DEVMODEW mode{};
	mode.dmSize = sizeof(mode);
	std::wstring name(d.name.begin(), d.name.end()); // device names are ASCII
	if (!EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &mode))
	{
		if (error)
			*error = "cannot read the display mode";
		return false;
	}
	const DWORD wanted = portrait ? DMDO_90 : DMDO_DEFAULT;
	if (mode.dmDisplayOrientation == wanted)
		return true;
	mode.dmDisplayOrientation = wanted;
	// Swap the mode's width/height as required by ChangeDisplaySettingsEx.
	if (portrait)
		std::swap(mode.dmPelsWidth, mode.dmPelsHeight);
	if (ChangeDisplaySettingsExW(name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY, nullptr) != DISP_CHANGE_SUCCESSFUL)
	{
		if (error)
			*error = "the driver refused the rotation";
		return false;
	}
	return true;
}

std::optional<desktop_stream_setup> display_manager::open(const display_info & d, QString * error)
{
	auto all = displays();
	for (size_t i = 0; i < all.size(); ++i)
		if (all[i].name == d.name)
		{
			desktop_stream_setup setup;
			setup.windows = true;
			setup.monitor_index = int(i);
			setup.name = d.name;
			setup.bounds = {d.x, d.y, d.width, d.height};
			setup.expected_width = d.pixel_width;
			setup.expected_height = d.pixel_height;
			return setup;
		}
	if (error)
		*error = "the monitor is gone";
	return std::nullopt;
}
} // namespace od
