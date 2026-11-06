#include "app_paths.h"
#include "runtime_registration.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace fs = std::filesystem;

namespace od::openxr_runtime
{
namespace
{
fs::path xdg(const char * var, const char * fallback)
{
	const char * v = getenv(var);
	if (v && *v)
		return v;
	const char * home = getenv("HOME");
	return fs::path(home ? home : "/tmp") / fallback;
}

fs::path config_dir() { return xdg("XDG_CONFIG_HOME", ".config") / "openxr" / "1"; }
fs::path install_dir() { return xdg("XDG_DATA_HOME", ".local/share") / "opendesk" / "openxr"; }
fs::path backup_dir() { return xdg("XDG_STATE_HOME", ".local/state") / "opendesk" / "openxr-backup"; }

const char * active_names[] = {"active_runtime.json", "active_runtime.x86_64.json"};

// back up a regular file or a symlink (as a symlink) - never follow it
void save(const fs::path & from, const fs::path & to)
{
	std::error_code ec;
	auto st = fs::symlink_status(from, ec);
	if (ec || !fs::exists(st))
		return;
	fs::remove(to, ec);
	if (fs::is_symlink(st))
		fs::create_symlink(fs::read_symlink(from, ec), to, ec);
	else
		fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}
} // namespace

std::string installed_manifest()
{
	return (install_dir() / "opendesk_xr.json").string();
}

bool active()
{
	std::error_code ec;
	for (const char * name: active_names)
	{
		fs::path p = config_dir() / name;
		if (!fs::is_symlink(p, ec) || fs::read_symlink(p, ec) != fs::path(installed_manifest()))
			return false;
	}
	return true;
}

std::string activate(const std::string & runtime_dir)
{
	std::error_code ec;
	fs::create_directories(install_dir(), ec);
	for (const char * lib: {"libopendesk-openxr.so", "libovr-vd.so"})
	{
		fs::path src = fs::path(runtime_dir) / lib;
		if (!fs::exists(src))
			return "OpenDesk XR is not built (" + src.string() + " missing)";
		// copy to a temp name and rename: apps that have the old copy loaded keep working
		fs::path tmp = install_dir() / (std::string(lib) + ".new");
		fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
		if (!ec)
			fs::rename(tmp, install_dir() / lib, ec);
		if (ec)
			return "cannot install " + std::string(lib) + ": " + ec.message();
	}
	{
		std::ofstream m(installed_manifest());
		m << "{\n  \"file_format_version\": \"1.0.0\",\n  \"runtime\": {\n    \"library_path\": \""
		  << (install_dir() / "libopendesk-openxr.so").string() << "\",\n    \"name\": \"OpenDesk XR (Linux)\"\n  }\n}\n";
	}
	if (active())
		return {};
	fs::create_directories(config_dir(), ec);
	fs::create_directories(backup_dir(), ec);
	bool have_backup = fs::exists(backup_dir() / ".saved", ec);
	for (const char * name: active_names)
	{
		fs::path p = config_dir() / name;
		if (!have_backup)
			save(p, backup_dir() / name);
		fs::remove(p, ec);
		fs::create_symlink(installed_manifest(), p, ec);
		if (ec)
			return "cannot set " + p.string() + ": " + ec.message();
	}
	std::ofstream(backup_dir() / ".saved") << "the user's OpenXR runtime before OpenDesk Streamer took over\n";
	return {};
}

void restore()
{
	std::error_code ec;
	if (!fs::exists(backup_dir() / ".saved", ec))
		return;
	for (const char * name: active_names)
	{
		fs::path p = config_dir() / name;
		// only undo our own change: leave it if something else replaced it meanwhile
		if (fs::is_symlink(p, ec) && fs::read_symlink(p, ec) == fs::path(installed_manifest()))
		{
			fs::remove(p, ec);
			fs::path b = backup_dir() / name;
			if (fs::exists(fs::symlink_status(b, ec)))
				save(b, p);
		}
		fs::remove(backup_dir() / name, ec);
	}
	fs::remove(backup_dir() / ".saved", ec);
}
} // namespace od::openxr_runtime

namespace od::openvr_runtime
{
namespace
{
fs::path xdg(const char * var, const char * fallback)
{
	const char * v = getenv(var);
	if (v && *v)
		return v;
	const char * home = getenv("HOME");
	return fs::path(home ? home : "/tmp") / fallback;
}

fs::path paths_file() { return xdg("XDG_CONFIG_HOME", ".config") / "openvr" / "openvrpaths.vrpath"; }
fs::path backup_dir() { return xdg("XDG_STATE_HOME", ".local/state") / "opendesk" / "openvr-backup"; }

bool is_runtime(const fs::path & dir)
{
	std::error_code ec;
	return fs::exists(dir / "bin" / "linux64" / "vrclient.so", ec);
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
	tmp += ".vd-new";
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
	return (xdg("XDG_DATA_HOME", ".local/share") / "opendesk" / "xrizer").string();
}

std::string built_xrizer_dir()
{
	return (xdg("XDG_DATA_HOME", ".local/share") / "opendesk" / "xrizer-build").string();
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
	if (const char * env = getenv("OPENDESK_XRIZER"); env && *env)
		return is_runtime(env) ? std::string(env) : std::string();
	const auto bundled = od::resource_dir("xrizer", "").toStdString();
	if (!bundled.empty() && is_runtime(bundled)) return bundled;
	return is_runtime(built_xrizer_dir()) ? built_xrizer_dir() : std::string();
}

bool active()
{
	QJsonArray runtime = read_paths().value("runtime").toArray();
	return !runtime.isEmpty() && runtime.first().toString().toStdString() == installed_dir();
}

std::string activate(const std::string & xrizer_dir)
{
	std::error_code ec;
	fs::path dst = installed_dir();
	if (!is_runtime(xrizer_dir))
		return "no xrizer runtime in " + xrizer_dir;
	// refresh the copy (temp dir + rename: apps with the old vrclient.so loaded keep their files)
	fs::path tmp = dst;
	tmp += ".new";
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);
	fs::copy(fs::path(xrizer_dir) / "bin", tmp / "bin", fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
	if (ec)
		return "cannot copy xrizer: " + ec.message();
	fs::path old = dst;
	old += ".old";
	fs::remove_all(old, ec);
	if (fs::exists(dst, ec))
		fs::rename(dst, old, ec);
	fs::rename(tmp, dst, ec);
	if (ec)
		return "cannot install xrizer: " + ec.message();
	fs::remove_all(old, ec);
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
	o["runtime"] = QJsonArray{QString::fromStdString(dst.string())};
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
void remove_leftovers()
{
	const char * v = getenv("XDG_DATA_HOME");
	const char * home = getenv("HOME");
	fs::path data = v && *v ? fs::path(v) : fs::path(home ? home : "/tmp") / ".local/share";
	std::error_code ec;
	fs::remove(data / "vulkan" / "implicit_layer.d" / "VkLayer_vd_foveated.json", ec);
	fs::remove(data / "opendesk" / "vulkan" / "libVkLayer_vd_foveated.so", ec);
	fs::remove(data / "opendesk" / "vulkan", ec); // only if empty
}
} // namespace od::foveated_layer
