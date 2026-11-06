// Windows side of runtime_registration.h.
//
// OpenXR: the loader reads only HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime (no per-user setting), which takes
// administrator rights. The runtime is copied to %LOCALAPPDATA%\OpenDesk\openxr (a running game keeps its copy when the
// streamer updates) and registered there. activate() writes the value directly when it may (the streamer runs
// elevated, or the installer granted the key), else asks once per run through UAC (reg.exe, elevated) and reports
// "pending: ..." until the value is ours. The previous value is kept in HKCU\Software\OpenDesk\OpenXR\PreviousRuntime; restore() puts
// it back only when that needs no new prompt - like the "OpenXR Runtime" option of other streamers, OpenDesk XR stays the
// active runtime otherwise.
//
// OpenVR: per user, no rights needed: %LOCALAPPDATA%\openvr\openvrpaths.vrpath, swapped while the streamer runs and
// restored when it exits, exactly as on Linux. xrizer's Windows build lives in bin\win64\vrclient_x64.dll.
#ifdef _WIN32
#include "app_paths.h"
#include "runtime_registration.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdlib>
#include <filesystem>
#include <thread>
#include <fstream>
#include <sstream>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace fs = std::filesystem;

namespace
{
fs::path local_appdata()
{
	PWSTR path = nullptr;
	fs::path out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
		out = path;
	CoTaskMemFree(path);
	if (out.empty())
		if (const char * v = std::getenv("LOCALAPPDATA"))
			out = v;
	return out;
}

std::wstring read_string(HKEY root, const wchar_t * key, const wchar_t * name, REGSAM view = KEY_WOW64_64KEY)
{
	wchar_t buffer[2048]{};
	DWORD size = sizeof(buffer) - sizeof(wchar_t);
	if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | (view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY : 0), nullptr, buffer, &size) !=
	    ERROR_SUCCESS)
		return {};
	return buffer;
}

bool write_string(HKEY root, const wchar_t * key, const wchar_t * name, const std::wstring & value)
{
	HKEY k = nullptr;
	if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS)
		return false;
	const LSTATUS r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
	                                 DWORD((value.size() + 1) * sizeof(wchar_t)));
	RegCloseKey(k);
	return r == ERROR_SUCCESS;
}

bool same_path(const std::wstring & a, const std::wstring & b)
{
	std::error_code ec;
	return !a.empty() && !b.empty() && (fs::path(a) == fs::path(b) || fs::equivalent(a, b, ec));
}

constexpr const wchar_t * khronos_key = L"SOFTWARE\\Khronos\\OpenXR\\1";
constexpr const wchar_t * our_key = L"Software\\OpenDesk\\OpenXR";

// reg.exe in an elevated process: one UAC prompt (on its own thread: the engine keeps running while the user decides)
bool elevated_set_active_runtime(const std::wstring & manifest)
{
	std::wstring args = L"add \"HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\" /v ActiveRuntime /t REG_SZ /d \"" + manifest + L"\" /f /reg:64";
	SHELLEXECUTEINFOW info{sizeof(info)};
	info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE | SEE_MASK_FLAG_NO_UI;
	info.lpVerb = L"runas";
	info.lpFile = L"reg.exe";
	info.lpParameters = args.c_str();
	info.nShow = SW_HIDE;
	if (!ShellExecuteExW(&info) || !info.hProcess)
		return false; // declined, or no UI
	WaitForSingleObject(info.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(info.hProcess, &code);
	CloseHandle(info.hProcess);
	return code == 0;
}
} // namespace

namespace od::openxr_runtime
{
namespace
{
fs::path install_dir()
{
	return local_appdata() / "OpenDesk" / "openxr";
}
bool elevation_asked = false; // once per run: the user said no, or the prompt is up
} // namespace

std::string installed_manifest()
{
	return (install_dir() / "opendesk_xr.json").string();
}

bool active()
{
	const std::wstring current = read_string(HKEY_LOCAL_MACHINE, khronos_key, L"ActiveRuntime");
	return same_path(current, fs::path(installed_manifest()).wstring()) ||
	       same_path(current, (fs::path(od::resource_dir("runtime", "").toStdWString()) / L"opendesk_xr.json").wstring());
}

std::string activate(const std::string & runtime_dir)
{
	std::error_code ec;
	// the installer made the installed runtime the active one (it runs elevated): nothing to do, no prompt
	if (same_path(read_string(HKEY_LOCAL_MACHINE, khronos_key, L"ActiveRuntime"), (fs::path(runtime_dir) / "opendesk_xr.json").wstring()))
		return {};
	fs::create_directories(install_dir(), ec);
	for (const char * lib: {"opendesk-openxr.dll", "ovr-vd.dll"})
	{
		fs::path src = fs::path(runtime_dir) / lib;
		if (!fs::exists(src))
			return "OpenDesk XR is not built (" + src.string() + " missing)";
		// a game may have the old copy loaded (Windows locks it): move it aside first, then copy
		fs::path dst = install_dir() / lib;
		fs::path old = dst;
		old += ".old";
		fs::remove(old, ec);
		if (fs::exists(dst))
			fs::rename(dst, old, ec);
		fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
		if (ec)
			return "cannot install " + std::string(lib) + ": " + ec.message();
		fs::remove(old, ec); // fails while a game still runs it; removed next time
	}
	// the DLLs the runtime needs that Windows doesn't ship (C++ runtime of the build)
	for (const auto & e: fs::directory_iterator(runtime_dir, ec))
		if (e.path().extension() == ".dll" && e.path().filename() != "opendesk-openxr.dll" && e.path().filename() != "ovr-vd.dll")
			fs::copy_file(e.path(), install_dir() / e.path().filename(), fs::copy_options::update_existing, ec);
	{
		std::ofstream m(installed_manifest());
		m << "{\n  \"file_format_version\": \"1.0.0\",\n  \"runtime\": {\n    \"library_path\": \".\\\\opendesk-openxr.dll\",\n"
		  << "    \"name\": \"OpenDesk XR\"\n  }\n}\n";
	}
	if (active())
		return {};
	const std::wstring manifest = fs::path(installed_manifest()).wstring();
	const std::wstring previous = read_string(HKEY_LOCAL_MACHINE, khronos_key, L"ActiveRuntime");
	if (!previous.empty() && !same_path(previous, manifest))
		write_string(HKEY_CURRENT_USER, our_key, L"PreviousRuntime", previous);
	if (write_string(HKEY_LOCAL_MACHINE, khronos_key, L"ActiveRuntime", manifest))
		return {};
	if (elevation_asked)
		return "OpenDesk XR is not the active OpenXR runtime (administrator approval was not given; set it on the Setup page)";
	elevation_asked = true;
	std::thread([manifest] { elevated_set_active_runtime(manifest); }).detach(); // the next update sees the result
	return "pending: Windows asks for administrator approval to make OpenDesk XR the active OpenXR runtime";
}

void restore()
{
	if (!active())
		return;
	const std::wstring previous = read_string(HKEY_CURRENT_USER, our_key, L"PreviousRuntime");
	if (previous.empty())
		return;
	// no prompt on the way out: only when the key is writable without one
	if (write_string(HKEY_LOCAL_MACHINE, khronos_key, L"ActiveRuntime", previous))
		RegDeleteKeyValueW(HKEY_CURRENT_USER, our_key, L"PreviousRuntime");
}
} // namespace od::openxr_runtime

namespace od::openvr_runtime
{
namespace
{
fs::path paths_file()
{
	return local_appdata() / "openvr" / "openvrpaths.vrpath";
}
fs::path backup_dir()
{
	return local_appdata() / "OpenDesk" / "openvr-backup";
}

bool is_runtime(const fs::path & dir)
{
	std::error_code ec;
	return fs::exists(dir / "bin" / "win64" / "vrclient_x64.dll", ec);
}

QJsonObject read_paths()
{
	std::ifstream in(paths_file());
	std::stringstream ss;
	ss << in.rdbuf();
	return QJsonDocument::fromJson(QByteArray::fromStdString(ss.str())).object();
}

bool write_paths(const QJsonObject & o)
{
	std::error_code ec;
	fs::create_directories(paths_file().parent_path(), ec);
	fs::path tmp = paths_file();
	tmp += ".od-new";
	{
		std::ofstream out(tmp);
		if (!out)
			return false;
		out << QJsonDocument(o).toJson(QJsonDocument::Indented).toStdString();
	}
	fs::rename(tmp, paths_file(), ec);
	return !ec;
}
} // namespace

std::string installed_dir()
{
	return (local_appdata() / "OpenDesk" / "xrizer").string();
}

std::string built_xrizer_dir()
{
	return (local_appdata() / "OpenDesk" / "xrizer-build").string();
}

std::string xrizer_version()
{
	std::ifstream v(fs::path(built_xrizer_dir()) / "version.txt");
	std::string line;
	std::getline(v, line);
	return is_runtime(built_xrizer_dir()) ? (line.empty() ? std::string("unknown") : line) : std::string();
}

std::string find_xrizer()
{
	if (const char * env = std::getenv("OPENDESK_XRIZER"); env && *env)
		return is_runtime(env) ? std::string(env) : std::string();
	const auto bundled = od::resource_dir("xrizer", "").toStdString();
	if (!bundled.empty() && is_runtime(bundled))
		return bundled;
	return is_runtime(built_xrizer_dir()) ? built_xrizer_dir() : std::string();
}

bool active()
{
	QJsonArray runtime = read_paths().value("runtime").toArray();
	return !runtime.isEmpty() && same_path(runtime.first().toString().toStdWString(), fs::path(installed_dir()).wstring());
}

std::string activate(const std::string & xrizer_dir)
{
	std::error_code ec;
	fs::path dst = installed_dir();
	if (!is_runtime(xrizer_dir))
		return "no xrizer runtime in " + xrizer_dir;
	// a running game has vrclient_x64.dll loaded (locked): install the new copy beside it and switch directories
	fs::path tmp = dst;
	tmp += ".new";
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	fs::copy(fs::path(xrizer_dir) / "bin", tmp / "bin", fs::copy_options::recursive, ec);
	if (ec)
		return "cannot copy xrizer: " + ec.message();
	fs::path old = dst;
	old += ".old";
	fs::remove_all(old, ec);
	if (fs::exists(dst, ec))
		fs::rename(dst, old, ec);
	if (ec) // locked by a running game: keep using the installed copy
		fs::remove_all(tmp, ec);
	else
	{
		fs::rename(tmp, dst, ec);
		if (ec)
			return "cannot install xrizer: " + ec.message();
		fs::remove_all(old, ec);
	}
	if (active())
		return {};

	fs::create_directories(backup_dir(), ec);
	if (!fs::exists(backup_dir() / ".saved", ec))
	{
		if (fs::exists(paths_file(), ec))
			fs::copy_file(paths_file(), backup_dir() / "openvrpaths.vrpath", fs::copy_options::overwrite_existing, ec);
		std::ofstream(backup_dir() / ".saved") << "the user's openvrpaths.vrpath before OpenDesk Streamer took over\n";
	}
	QJsonObject o = read_paths();
	if (o.isEmpty())
		o = {{"jsonid", "vrpathreg"}, {"version", 1}, {"external_drivers", QJsonValue::Null}};
	o["runtime"] = QJsonArray{QString::fromStdWString(dst.wstring())};
	if (!write_paths(o))
		return "cannot write " + paths_file().string();
	return {};
}

void restore()
{
	std::error_code ec;
	if (!fs::exists(backup_dir() / ".saved", ec))
		return;
	if (active()) // only undo our own change
	{
		if (fs::exists(backup_dir() / "openvrpaths.vrpath", ec))
			fs::copy_file(backup_dir() / "openvrpaths.vrpath", paths_file(), fs::copy_options::overwrite_existing, ec);
		else
			fs::remove(paths_file(), ec);
	}
	fs::remove(backup_dir() / "openvrpaths.vrpath", ec);
	fs::remove(backup_dir() / ".saved", ec);
}
} // namespace od::openvr_runtime

namespace od::foveated_layer
{
void remove_leftovers() {} // the removed Vulkan layer was never installed on Windows
} // namespace od::foveated_layer
#endif
