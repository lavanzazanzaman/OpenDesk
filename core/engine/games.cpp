#include "games.h"
#include "platform.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace od
{
namespace
{
QString data_home()
{
	return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
}

// ---------------------------------------------------------------------------------------------------------------
// RiftLift

uint64_t fnv1a(const QByteArray & s)
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (char c: s)
		h = (h ^ uint8_t(c)) * 0x100000001b3ull;
	return h;
}

void scan_riftlift(std::vector<game_entry> & out, std::set<uint64_t> & steam_ids_taken)
{
	QDir dir(data_home() + "/riftlift/games");
	for (const QFileInfo & f: dir.entryInfoList({"*.json"}, QDir::Files, QDir::Name))
	{
		QFile file(f.filePath());
		if (!file.open(QIODevice::ReadOnly))
			continue;
		QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
		QString slug = o.value("slug").toString(f.completeBaseName());
		QString directory = o.value("directory").toString();
		if (slug.isEmpty() || (!directory.isEmpty() && !QFileInfo(directory).isDir()))
			continue; // not installed (any more)
		game_entry g;
		g.platform = game_platform::oculus_rift;
		bool numeric = false;
		g.app_id = o.value("app_id").toString().toULongLong(&numeric);
		if (!numeric || !g.app_id)
			g.app_id = fnv1a(slug.toUtf8()) & 0x7fffffffffffffffull;
		g.name = o.value("name").toString(slug);
		g.riftlift_slug = slug;
		g.riftlift_key = o.value("app_key").toString(slug);
		QJsonObject art = o.value("artwork").toObject();
		QString art_dir = data_home() + "/riftlift/artwork/" + slug + "/";
		if (art.contains("grid"))
			g.art.push_back(art.value("grid").toString());
		g.art.push_back(art_dir + "grid.png");
		g.hero = art.value("hero").toString(art_dir + "hero.png");
		g.logo = art.value("logo").toString(art_dir + "logo.png");
		// a Steam-distributed Oculus build: Steam itself can't start it in VR, RiftLift can
		if (o.value("source").toString() == "steam")
			if (uint64_t sid = uint64_t(o.value("steam_app_id").toDouble()); sid && sid < (1ull << 32))
				steam_ids_taken.insert(sid);
		out.push_back(std::move(g));
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Steam

QString steam_root()
{
	return platform::steam_root();
}

#ifdef _WIN32
// ---------------------------------------------------------------------------------------------------------------
// Oculus (Meta) store: OculusBase\Manifests\*.json plus every user's Oculus library folders, exactly the places
// the original Windows streamer scans. The game is started from its own executable (OculusClient is started first
// when it is not running), and a game counts as running while a process runs from its software folder.

QSettings oculus_registry(const QString & path)
{
	return QSettings(path, QSettings::NativeFormat);
}

QString oculus_base();

QString oculus_client_executable()
{
	return QDir::cleanPath(oculus_base() + "/Support/oculus-client/OculusClient.exe");
}

QString oculus_base()
{
	QString base = oculus_registry("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Oculus VR, LLC\\Oculus").value("Base").toString();
	if (base.isEmpty())
		base = "C:\\Program Files\\Oculus";
	return QDir::toNativeSeparators(base);
}

QStringList oculus_manifest_files()
{
	QStringList out;
	auto add = [&](const QString & dir) {
		for (const QFileInfo & f: QDir(dir).entryInfoList({"*.json"}, QDir::Files))
			out << f.absoluteFilePath();
	};
	const QString base = oculus_base();
	add(base + "/Manifests");
	// Per-user libraries, like the original: every loaded user hive under HKEY_USERS (which includes HKCU).
	auto add_libraries = [&](QSettings & libs) {
		for (const QString & group: libs.childGroups())
		{
			libs.beginGroup(group);
			QString path = libs.value("OriginalPath").toString();
			if (path.isEmpty())
				path = libs.value("Path").toString();
			libs.endGroup();
			if (!path.isEmpty())
				add(QDir::toNativeSeparators(path) + "/Manifests");
		}
	};
	QSettings users("HKEY_USERS", QSettings::NativeFormat);
	for (const QString & sid: users.childGroups())
	{
		if (sid.endsWith("_Classes"))
			continue;
		QSettings libs("HKEY_USERS\\" + sid + "\\Software\\Oculus VR, LLC\\Oculus\\Libraries", QSettings::NativeFormat);
		add_libraries(libs);
	}
	QSettings user("HKEY_CURRENT_USER\\Software\\Oculus VR, LLC\\Oculus\\Libraries", QSettings::NativeFormat);
	add_libraries(user);
	return out;
}

void scan_oculus(std::vector<game_entry> & out)
{
	QStringList seen;
	for (QString manifest: oculus_manifest_files())
	{
		if (QFileInfo::exists(manifest + ".mini"))
			manifest += ".mini"; // a store update's partial manifest takes precedence
		QFile f(manifest);
		if (!f.open(QIODevice::ReadOnly))
			continue;
		QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
		QString launch_file = o.value("launchFile").toString();
		QString canonical = o.value("canonicalName").toString();
		if (launch_file.isEmpty() || canonical.isEmpty() || seen.contains(canonical))
			continue;
		// appId is a decimal string in modern manifests; older ones may omit it
		bool numeric = false;
		uint64_t app_id = o.value("appId").toString().toULongLong(&numeric);
		if (!numeric || !app_id)
			app_id = fnv1a(canonical.toUtf8()) & 0x7fffffffffffffffull;
		seen << canonical;
		game_entry g;
		g.platform = game_platform::oculus_rift;
		g.app_id = app_id;
		g.name = canonical;
		g.launch_args = o.value("launchParameters").toString();
		const QString library = QFileInfo(manifest).absolutePath() + "/..";
		const QString software = QDir::cleanPath(library + "/Software/" + canonical);
		QString exe = QDir::cleanPath(software + "/" + QFileInfo(launch_file).fileName());
		if (!QFileInfo::exists(exe))
		{
			// some manifests carry a relative launchFile with subfolders
			QString joined = QDir::cleanPath(software + "/" + launch_file);
			if (QFileInfo::exists(joined))
				exe = joined;
		}
		g.launch_exe = exe;
		g.install_dir = software;
		QString shared = QDir::cleanPath(oculus_base() + "/CoreData/Software/StoreAssets/" + canonical + "_assets/cover_landscape_image.jpg");
		if (QFileInfo::exists(shared))
			g.art.push_back(shared);
		QString art = QDir::cleanPath(library + "/Software/StoreAssets/" + canonical + "_assets/cover_landscape_image.jpg");
		if (art != shared && QFileInfo::exists(art))
			g.art.push_back(art);
		out.push_back(std::move(g));
	}
}
#endif

QString read_text(const QString & path)
{
	QFile f(path);
	return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QString vdf_unescape(QString s)
{
	return s.replace("\\\\", "\\").replace("\\\"", "\"");
}

// binary KeyValues (appinfo.vdf): 0 = subtree, 1 = string, 2 = int32, 7 = uint64, 8 = end
struct kv_node
{
	std::string value;
	std::map<std::string, kv_node> children;
	const kv_node * get(const char * key) const
	{
		auto it = children.find(key);
		return it == children.end() ? nullptr : &it->second;
	}
	std::string str(const char * key) const
	{
		const kv_node * n = get(key);
		return n ? n->value : std::string();
	}
};

class appinfo_reader
{
public:
	explicit appinfo_reader(QByteArray data) : d(std::move(data)) {}

	// calls visit(app id, kv root) for each app in `wanted`
	bool read(const std::set<uint32_t> & wanted, const std::function<void(uint32_t, const kv_node &)> & visit)
	{
		uint32_t magic;
		if (!u32(0, magic))
			return false;
		int version = int(magic & 0xff);
		if ((magic >> 8) != 0x075644 || version < 0x27 || version > 0x29)
			return false; // unknown layout
		size_t pos = 8;
		if (version >= 0x29)
		{
			uint64_t table;
			if (!u64(8, table) || !read_string_table(table))
				return false;
			pos = 16;
		}
		const size_t header = 4 + 4 + 8 + 20 + 4 + (version >= 0x28 ? 20 : 0);
		for (;;)
		{
			uint32_t appid, size;
			if (!u32(pos, appid) || appid == 0 || !u32(pos + 4, size))
				break;
			size_t body = pos + 8;
			if (body + size > size_t(d.size()))
				return false;
			if (wanted.count(appid))
			{
				kv_node root;
				size_t p = body + header;
				key_table = version >= 0x29;
				if (parse(p, body + size, root, 0))
					visit(appid, root.children.count("appinfo") ? root.children["appinfo"] : root);
			}
			pos = body + size;
		}
		return true;
	}

private:
	bool u32(size_t p, uint32_t & v) const
	{
		if (p + 4 > size_t(d.size()))
			return false;
		std::memcpy(&v, d.constData() + p, 4); // little-endian file, x86 host
		return true;
	}
	bool u64(size_t p, uint64_t & v) const
	{
		if (p + 8 > size_t(d.size()))
			return false;
		std::memcpy(&v, d.constData() + p, 8);
		return true;
	}
	bool cstring(size_t & p, size_t end, std::string & s) const
	{
		const char * b = d.constData() + p;
		const void * z = std::memchr(b, 0, end - p);
		if (!z)
			return false;
		s.assign(b, static_cast<const char *>(z));
		p += s.size() + 1;
		return true;
	}
	bool read_string_table(uint64_t off)
	{
		uint32_t count;
		if (!u32(off, count))
			return false;
		size_t p = off + 4;
		strings.reserve(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			std::string s;
			if (!cstring(p, d.size(), s))
				return false;
			strings.push_back(std::move(s));
		}
		return true;
	}
	bool parse(size_t & p, size_t end, kv_node & node, int depth)
	{
		if (depth > 32)
			return false;
		while (p < end)
		{
			uint8_t type = uint8_t(d[qsizetype(p++)]);
			if (type == 8)
				return true;
			std::string key;
			if (key_table)
			{
				uint32_t index;
				if (!u32(p, index) || index >= strings.size())
					return false;
				key = strings[index];
				p += 4;
			}
			else if (!cstring(p, end, key))
				return false;
			kv_node & child = node.children[key];
			switch (type)
			{
				case 0:
					if (!parse(p, end, child, depth + 1))
						return false;
					break;
				case 1:
					if (!cstring(p, end, child.value))
						return false;
					break;
				case 2:
				{
					uint32_t v;
					if (!u32(p, v))
						return false;
					child.value = std::to_string(int32_t(v));
					p += 4;
					break;
				}
				case 7:
				{
					uint64_t v;
					if (!u64(p, v))
						return false;
					child.value = std::to_string(v);
					p += 8;
					break;
				}
				default:
					return false;
			}
		}
		return false;
	}

	QByteArray d;
	std::vector<std::string> strings;
	bool key_table = false;
};

bool flag(const kv_node & common, const char * key)
{
	std::string v = common.str(key);
	return !v.empty() && v != "0";
}

// what the Steam store calls "VR Only" / "VR Supported"
bool is_vr_app(const kv_node & app)
{
	const kv_node * common = app.get("common");
	if (!common)
		return false;
	QString type = QString::fromStdString(common->str("type")).toLower();
	if (type != "game" && type != "demo" && type != "application" && type != "mod")
		return false; // SteamVR itself is a Tool
	if (flag(*common, "onlyvrsupport") || flag(*common, "openvrsupport") || flag(*common, "openxrsupport") ||
	    flag(*common, "othervrsupport"))
		return true;
	if (const kv_node * launch = app.get("config") ? app.get("config")->get("launch") : nullptr)
		for (const auto & [_, l]: launch->children)
		{
			std::string t = l.str("type");
			const kv_node * cfg = l.get("config");
			if ((t == "vr" || t == "openxr" || t == "othervr") && (!cfg || cfg->str("oslist") != "android"))
				return true;
		}
	return false;
}

void scan_steam(std::vector<game_entry> & out, const std::set<uint64_t> & taken)
{
	QString root = steam_root();
	if (root.isEmpty())
		return;
	struct installed_app
	{
		QString name;
		QString dir; // <library>/steamapps/common/<installdir>
	};
	std::map<uint32_t, installed_app> installed; // app id -> name and folder
	static const QRegularExpression path_re(R"re("path"\s+"((?:[^"\\]|\\.)*)")re");
	static const QRegularExpression appid_re(R"re("appid"\s+"(\d+)")re");
	static const QRegularExpression name_re(R"re("name"\s+"((?:[^"\\]|\\.)*)")re");
	static const QRegularExpression installdir_re(R"re("installdir"\s+"((?:[^"\\]|\\.)*)")re");
	static const QRegularExpression state_re(R"re("StateFlags"\s+"(\d+)")re");
	QStringList libraries{root};
	for (auto it = path_re.globalMatch(read_text(root + "/steamapps/libraryfolders.vdf")); it.hasNext();)
	{
		QString lib = vdf_unescape(it.next().captured(1));
		if (!libraries.contains(lib) && QFileInfo(lib).canonicalFilePath() != root)
			libraries << lib;
	}
	for (const QString & lib: libraries)
		for (const QFileInfo & m: QDir(lib + "/steamapps").entryInfoList({"appmanifest_*.acf"}, QDir::Files))
		{
			QString t = read_text(m.filePath());
			auto id = appid_re.match(t);
			auto state = state_re.match(t);
			if (!id.hasMatch() || (state.hasMatch() && !(state.captured(1).toUInt() & 4))) // 4 = fully installed
				continue;
			installed_app app;
			app.name = vdf_unescape(name_re.match(t).captured(1));
			QString folder = vdf_unescape(installdir_re.match(t).captured(1));
			if (!folder.isEmpty())
				app.dir = QDir::cleanPath(lib + "/steamapps/common/" + folder);
			installed[id.captured(1).toUInt()] = std::move(app);
		}
	if (installed.empty())
		return;

	QFile f(root + "/appcache/appinfo.vdf");
	if (!f.open(QIODevice::ReadOnly))
		return;
	std::set<uint32_t> wanted;
	for (const auto & [id, _]: installed)
		wanted.insert(id);
	std::set<uint32_t> vr;
	appinfo_reader(f.readAll()).read(wanted, [&](uint32_t id, const kv_node & app) {
		if (is_vr_app(app))
			vr.insert(id);
	});

	std::vector<game_entry> steam;
	for (uint32_t id: vr)
	{
		if (taken.count(id))
			continue;
		game_entry g;
		g.platform = game_platform::steam;
		g.app_id = id;
		g.name = installed[id].name;
		g.install_dir = installed[id].dir;
		// librarycache/<id>/[<hash>/]header.jpg etc. (newer clients put each asset in a hash folder)
		QString cache = root + "/appcache/librarycache/" + QString::number(id);
		std::map<QString, QString> found;
		for (QDirIterator it(cache, QDir::Files, QDirIterator::Subdirectories); it.hasNext();)
		{
			QString path = it.next();
			found.emplace(it.fileName(), path);
		}
		for (const char * n: {"header.jpg", "library_header.jpg"})
			if (found.count(n))
				g.art.push_back(found[n]);
		if (QString h = root + "/appcache/librarycache/" + QString::number(id) + "_header.jpg"; QFileInfo::exists(h))
			g.art.push_back(h); // older flat layout
		if (found.count("library_hero.jpg"))
			g.hero = found["library_hero.jpg"];
		if (found.count("logo.png"))
			g.logo = found["logo.png"];
		steam.push_back(std::move(g));
	}
	std::sort(steam.begin(), steam.end(), [](const game_entry & a, const game_entry & b) {
		return QString::localeAwareCompare(a.name, b.name) < 0;
	});
	for (auto & g: steam)
		out.push_back(std::move(g));
}

// ---------------------------------------------------------------------------------------------------------------
// thumbnails

constexpr int thumb_w = 220, thumb_h = 128; // the headset copies the image 1:1 into a 220x128 atlas cell

QImage load(const QString & path)
{
	QImage img;
	if (!path.isEmpty() && QFileInfo::exists(path))
		img.load(path);
	return img;
}

void draw_cover(QPainter & p, const QImage & img, const QRect & r)
{
	QSize s = img.size().scaled(r.size(), Qt::KeepAspectRatioByExpanding);
	QImage scaled = img.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	p.drawImage(r, scaled, QRect((s.width() - r.width()) / 2, (s.height() - r.height()) / 2, r.width(), r.height()));
}

void draw_fit(QPainter & p, const QImage & img, const QRect & r)
{
	QSize s = img.size().scaled(r.size(), Qt::KeepAspectRatio);
	QRect dst(r.x() + (r.width() - s.width()) / 2, r.y() + (r.height() - s.height()) / 2, s.width(), s.height());
	p.drawImage(dst, img.scaled(s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

// banner art is wider than the tile: show all of it, over a darkened fill of itself instead of black bars
void compose_banner(QPainter & p, const QImage & art)
{
	QRect tile(0, 0, thumb_w, thumb_h);
	draw_cover(p, art.scaled(art.width() / 8 + 1, art.height() / 8 + 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), tile);
	p.fillRect(tile, QColor(0, 0, 0, 150));
	double aspect = double(art.width()) / std::max(1, art.height());
	if (aspect > 1.9 || aspect < 1.55)
		draw_fit(p, art, tile);
	else
		draw_cover(p, art, tile); // ~ tile aspect (capsule_616x353, 16:9): fill
}

void compose_card(QPainter & p, const game_entry & g, const QImage & hero, const QImage & logo)
{
	QRect tile(0, 0, thumb_w, thumb_h);
	if (!hero.isNull())
		draw_cover(p, hero, tile);
	else
	{
		QLinearGradient bg(0, 0, thumb_w, thumb_h);
		bg.setColorAt(0, g.platform == game_platform::steam ? QColor(23, 42, 69) : QColor(40, 30, 70));
		bg.setColorAt(1, QColor(12, 14, 20));
		p.fillRect(tile, bg);
	}
	if (!logo.isNull())
	{
		draw_fit(p, logo, tile.adjusted(18, 16, -18, -16));
		return;
	}
	p.fillRect(QRect(0, thumb_h - 52, thumb_w, 52), QColor(0, 0, 0, hero.isNull() ? 0 : 160));
	QFont font = p.font();
	font.setBold(true);
	font.setPixelSize(17);
	p.setFont(font);
	p.setPen(Qt::white);
	QRect text = hero.isNull() ? tile.adjusted(10, 10, -10, -10) : QRect(10, thumb_h - 50, thumb_w - 20, 46);
	p.drawText(text, Qt::AlignCenter | Qt::TextWordWrap, g.name);
}

QStringList split_args(const QByteArray & cmdline)
{
	QStringList args;
	for (const QByteArray & a: cmdline.split('\0'))
		if (!a.isEmpty())
			args << QString::fromLocal8Bit(a);
	return args;
}
} // namespace

std::vector<game_entry> scan_games()
{
	std::vector<game_entry> games;
	std::set<uint64_t> taken;
#ifdef _WIN32
	scan_oculus(games);
#else
	scan_riftlift(games, taken);
#endif
	scan_steam(games, taken);
	return games;
}

QByteArray game_thumbnail(const game_entry & g)
{
	QImage tile(thumb_w, thumb_h, QImage::Format_RGB888);
	tile.fill(Qt::black);
	{
		QPainter p(&tile);
		p.setRenderHint(QPainter::Antialiasing);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		QImage art;
		for (const QString & a: g.art)
			if (art = load(a); !art.isNull())
				break;
		if (!art.isNull())
			compose_banner(p, art);
		else
			compose_card(p, g, load(g.hero), load(g.logo));
	}
	QByteArray png;
	QBuffer buffer(&png);
	buffer.open(QIODevice::WriteOnly);
	tile.save(&buffer, "PNG");
	return png;
}

#ifdef _WIN32
process_table::process_table()
{
	for (const auto & e: platform::processes())
	{
		process p;
		p.pid = e.pid;
		p.ppid = e.ppid;
		p.start = e.start;
		p.exe = e.exe;
		p.args = e.args;
		procs.push_back(std::move(p));
	}
}

std::vector<int> process_table::roots(const game_entry & g, const markers &) const
{
	// A game runs while a process's executable lives in its install folder. Steam and Oculus games both keep
	// their binaries under that folder, so this covers the game and every helper it starts.
	QString folder = QDir::fromNativeSeparators(g.install_dir);
	const QString wanted = folder.toLower();
	std::vector<int> out;
	if (wanted.isEmpty())
		return out;
	for (const auto & p: procs)
	{
		const QString exe = QDir::fromNativeSeparators(p.exe).toLower();
		if (exe.startsWith(wanted + "/") || exe == wanted)
			out.push_back(p.pid);
	}
	return out;
}

std::vector<int> process_table::launchers(const game_entry &) const
{
	return {};
}

process_table::markers process_table::launch_markers(const game_entry &) const
{
	return {};
}

process_table::snapshot process_table::take(const game_entry & g, const markers & known) const
{
	snapshot out;
	for (int pid: pids(g, known))
		for (const auto & p: procs)
			if (p.pid == pid)
				out.emplace_back(pid, p.start);
	return out;
}

std::vector<int> process_table::remaining(const snapshot & then, const markers &) const
{
	std::vector<int> out;
	for (const auto & p: procs)
		if (std::find(then.begin(), then.end(), std::pair<int, qulonglong>(p.pid, p.start)) != then.end())
			out.push_back(p.pid);
	return out;
}

bool process_table::running(const game_entry & g, const markers & known) const
{
	return !roots(g, known).empty();
}

std::vector<int> process_table::pids(const game_entry & g, const markers & known) const
{
	std::vector<int> out = roots(g, known);
	std::set<int> seen(out.begin(), out.end());
	for (size_t i = 0; i < out.size(); ++i)
		for (const auto & p: procs)
			if (p.ppid == out[i] && seen.insert(p.pid).second)
				out.push_back(p.pid);
	return out;
}

QString riftlift_executable()
{
	return {}; // RiftLift is Linux-only; Windows uses the Oculus store
}

QString oculus_client_executable()
{
	return {};
}
#else
process_table::process_table()
{
	for (const QFileInfo & e: QDir("/proc").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
	{
		bool ok = false;
		int pid = e.fileName().toInt(&ok);
		if (!ok)
			continue;
		QFile cmd(e.filePath() + "/cmdline");
		QFile stat(e.filePath() + "/stat");
		if (!cmd.open(QIODevice::ReadOnly) || !stat.open(QIODevice::ReadOnly))
			continue;
		QByteArray s = stat.readAll();
		qsizetype close = s.lastIndexOf(')'); // comm may contain spaces/parens
		QList<QByteArray> fields = s.mid(close + 2).split(' ');
		process p;
		p.pid = pid;
		p.ppid = fields.size() > 1 ? fields[1].toInt() : 0;
		p.start = fields.size() > 19 ? fields[19].toULongLong() : 0;
		p.args = split_args(cmd.readAll());
		p.exe = p.args.value(0);
		QFile env(e.filePath() + "/environ"); // other users' processes: not readable, and not ours to match anyway
		if (env.open(QIODevice::ReadOnly))
			for (const QByteArray & v: env.readAll().split('\0'))
				if (v.startsWith("RIFTLIFT_LAUNCH_ID="))
					p.riftlift_launch = v.mid(19);
				else if (v.startsWith("SteamAppId="))
					p.steam_app_id = v.mid(11);
		procs.push_back(std::move(p));
	}
}

std::vector<int> process_table::roots(const game_entry & g, const markers & known) const
{
	std::vector<int> out;
	if (g.platform == game_platform::steam)
	{
		// Steam starts every game under `reaper SteamLaunch AppId=<id> -- ...`
		// ... and Proton passes SteamAppId=<id> to everything it starts
		QByteArray id = QByteArray::number(qulonglong(g.app_id));
		QString tag = "AppId=" + QString::number(qulonglong(g.app_id));
		for (const auto & p: procs)
		{
			bool launch = false, match = p.steam_app_id == id;
			for (const auto & a: p.args)
			{
				launch |= a == "SteamLaunch";
				match |= launch && a == tag;
			}
			if (match)
				out.push_back(p.pid);
		}
		return out;
	}
	// `riftlift launch <slug>` (the wrapper) and `proton run RiftLiftLauncher.exe ... /app <key> ...` (the game)
	const QString slug = g.riftlift_slug, key = g.riftlift_key;
	for (const auto & p: procs)
	{
		bool match = false;
		for (size_t i = 0; i + 1 < p.args.size() && !match; ++i)
		{
			const QString & a = p.args[i];
			if (a == "launch" && p.args[i + 1] == slug && i >= 1 && p.args[i - 1].endsWith("riftlift"))
				match = true;
			if (a.compare("/app", Qt::CaseInsensitive) == 0 && p.args[i + 1] == key)
				for (const auto & b: p.args)
					match |= b.contains("RiftLiftLauncher.exe");
		}
		if (match)
			out.push_back(p.pid);
	}
	// everything of those launches, also what Wine reparented (wineserver, the game itself)
	markers launches = known;
	for (int pid: out)
		for (const auto & p: procs)
			if (p.pid == pid && !p.riftlift_launch.isEmpty())
				launches.insert(p.riftlift_launch);
	std::set<int> have(out.begin(), out.end());
	for (const auto & p: procs)
		if (launches.count(p.riftlift_launch) && have.insert(p.pid).second)
			out.push_back(p.pid);
	return out;
}

std::vector<int> process_table::launchers(const game_entry & g) const
{
	std::vector<int> out;
	if (g.platform != game_platform::oculus_rift)
		return out;
	const QString slug = g.riftlift_slug;
	for (const auto & p: procs)
		for (size_t i = 1; i + 1 < p.args.size(); ++i)
			if (p.args[i] == "launch" && p.args[i + 1] == slug && p.args[i - 1].endsWith("riftlift"))
			{
				out.push_back(p.pid);
				break;
			}
	return out;
}

process_table::markers process_table::launch_markers(const game_entry & g) const
{
	markers out;
	if (g.platform != game_platform::oculus_rift)
		return out;
	for (int pid: roots(g, {}))
		for (const auto & p: procs)
			if (p.pid == pid && !p.riftlift_launch.isEmpty())
				out.insert(p.riftlift_launch);
	return out;
}

process_table::snapshot process_table::take(const game_entry & g, const markers & known) const
{
	snapshot out;
	for (int pid: pids(g, known))
		for (const auto & p: procs)
			if (p.pid == pid)
				out.emplace_back(pid, p.start);
	return out;
}

std::vector<int> process_table::remaining(const snapshot & then, const markers & known) const
{
	std::vector<int> out;
	for (const auto & p: procs)
	{
		bool was = std::find(then.begin(), then.end(), std::pair<int, qulonglong>(p.pid, p.start)) != then.end();
		if (was || (!p.riftlift_launch.isEmpty() && known.count(p.riftlift_launch)))
			out.push_back(p.pid);
	}
	return out;
}

bool process_table::running(const game_entry & g, const markers & known) const
{
	return !roots(g, known).empty();
}

std::vector<int> process_table::pids(const game_entry & g, const markers & known) const
{
	std::vector<int> out = roots(g, known);
	std::set<int> seen(out.begin(), out.end());
	for (size_t i = 0; i < out.size(); ++i)
		for (const auto & p: procs)
			if (p.ppid == out[i] && seen.insert(p.pid).second)
				out.push_back(p.pid);
	return out;
}

QString riftlift_executable()
{
	QString exe = QStandardPaths::findExecutable("riftlift");
	if (exe.isEmpty())
		exe = QStandardPaths::findExecutable("riftlift", {QDir::homePath() + "/.local/bin"});
	return exe;
}
#endif
} // namespace od
