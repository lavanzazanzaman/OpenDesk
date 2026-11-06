#include "displays_win.h"

#include <QTimer>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

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
constexpr const wchar_t * vdd_pipe = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";

// the adapter behind a display ("\\.\DISPLAY3") is the Virtual Display Driver
bool is_virtual_display(const std::string & device)
{
	DISPLAY_DEVICEW dd{};
	dd.cb = sizeof(dd);
	for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd.cb = sizeof(dd))
		if (utf8(dd.DeviceName) == device)
			return std::wstring(dd.DeviceString).find(L"Virtual Display") != std::wstring::npos;
	return false;
}

std::vector<std::string> virtual_displays(const std::vector<display_info> & all)
{
	std::vector<std::string> out;
	for (const auto & d: all)
		if (is_virtual_display(d.name))
			out.push_back(d.name);
	return out;
}

// one command to the driver ("SETDISPLAYCOUNT 2"): UTF-16, one pipe message
bool vdd_command(const std::wstring & command)
{
	if (!WaitNamedPipeW(vdd_pipe, 1000))
		return false;
	HANDLE pipe = CreateFileW(vdd_pipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	if (pipe == INVALID_HANDLE_VALUE)
		return false;
	DWORD mode = PIPE_READMODE_MESSAGE;
	SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
	DWORD written = 0;
	const BOOL ok = WriteFile(pipe, command.c_str(), DWORD(command.size() * sizeof(wchar_t)), &written, nullptr);
	CloseHandle(pipe);
	return ok;
}

// sets a display's mode (not saved in the registry: it's the session's virtual monitor)
bool set_mode(const std::string & device, const display_manager::virtual_mode & m)
{
	const std::wstring name(device.begin(), device.end());
	DEVMODEW mode{};
	mode.dmSize = sizeof(mode);
	mode.dmPelsWidth = DWORD(m.width);
	mode.dmPelsHeight = DWORD(m.height);
	mode.dmDisplayFrequency = DWORD((m.refresh_mhz + 500) / 1000);
	mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
	if (ChangeDisplaySettingsExW(name.c_str(), &mode, nullptr, 0, nullptr) == DISP_CHANGE_SUCCESSFUL)
		return true;
	mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT; // the driver's refresh rates may not include ours
	return ChangeDisplaySettingsExW(name.c_str(), &mode, nullptr, 0, nullptr) == DISP_CHANGE_SUCCESSFUL;
}
} // namespace

display_manager::display_manager(QObject * parent) :
        QObject(parent)
{
}

display_manager::~display_manager()
{
	// the user's own count again (the driver keeps it in its settings)
	if (base_count >= 0 && !added.empty())
		vdd_command(std::format(L"SETDISPLAYCOUNT {}", base_count));
}

bool display_manager::can_create_virtual() const
{
	return WaitNamedPipeW(vdd_pipe, 0) || GetLastError() == ERROR_SEM_TIMEOUT;
}

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

std::optional<std::string> display_manager::add_virtual(virtual_mode m, QString * error)
{
	if (!can_create_virtual())
	{
		if (error)
			*error = "virtual monitors need the Virtual Display Driver (github.com/VirtualDrivers/Virtual-Display-Driver)";
		return std::nullopt;
	}
	if (pending && pending->isActive())
	{
		if (error)
			*error = "the previous virtual monitor is still being added";
		return std::nullopt;
	}
	pending_before = virtual_displays(displays());
	if (base_count < 0)
		base_count = int(pending_before.size());
	if (!vdd_command(std::format(L"SETDISPLAYCOUNT {}", pending_before.size() + 1)))
	{
		if (error)
			*error = "the Virtual Display Driver did not take the command";
		return std::nullopt;
	}
	// the driver restarts with one more monitor: wait for it without blocking the engine
	pending_mode = m;
	pending_polls = 0;
	if (!pending)
	{
		pending = new QTimer(this);
		pending->setInterval(200);
		connect(pending, &QTimer::timeout, this, &display_manager::poll_pending);
	}
	pending->start();
	return std::string();
}

void display_manager::poll_pending()
{
	for (const auto & name: virtual_displays(displays()))
		if (std::find(pending_before.begin(), pending_before.end(), name) == pending_before.end())
		{
			pending->stop();
			if (!set_mode(name, pending_mode))
				emit log(QString("[monitors] %1: %2x%3 is not in the Virtual Display Driver's resolution list, keeping its default")
				                 .arg(QString::fromStdString(name))
				                 .arg(pending_mode.width)
				                 .arg(pending_mode.height));
			added.push_back(name);
			emit log(QString("[monitors] virtual monitor %1 is ready").arg(QString::fromStdString(name)));
			emit virtual_ready(QString::fromStdString(name));
			emit displays_changed();
			return;
		}
	if (++pending_polls >= 50) // 10 s
	{
		pending->stop();
		emit log("[monitors] the Virtual Display Driver added no monitor");
	}
}

bool display_manager::remove_virtual()
{
	if (pending && pending->isActive())
		pending->stop(); // one asked for but not there yet: the count goes back down below
	if (added.empty())
		return false;
	const auto now = virtual_displays(displays());
	if (now.empty() || !vdd_command(std::format(L"SETDISPLAYCOUNT {}", now.size() - 1)))
		return false;
	added.pop_back(); // the driver drops its last monitor
	emit displays_changed();
	return true;
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
