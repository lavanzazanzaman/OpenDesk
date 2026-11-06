#ifdef _WIN32
#include "dxvk_win.h"
#include "app_paths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <windows.h>

namespace od::dxvk
{
namespace
{
const QStringList dlls{"d3d11.dll", "dxgi.dll"};
const char * marker = ".opendesk-dxvk";

// executables that are not the game (installers, crash reporters, launchers' helpers)
bool is_game_exe(const QFileInfo & f)
{
	static const QStringList skip{"unitycrashhandler", "crashreport", "crashpad", "vcredist", "dxsetup", "dotnet", "uninstall", "setup",
	                              "unins0", "easyanticheat", "battleye", "be_service", "eac", "ue4prereq", "prereq", "redist"};
	const QString name = f.fileName().toLower();
	for (const auto & s: skip)
		if (name.contains(s))
			return false;
	return true;
}

QByteArray digest(const QString & path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	QCryptographicHash h(QCryptographicHash::Sha256);
	h.addData(&f);
	return h.result().toHex();
}

// directories holding the game's executables (Unity: the root, Unreal: Binaries\Win64, ...), at most 4 levels down
QStringList exe_dirs(const QString & root)
{
	QSet<QString> out;
	const int base_depth = QDir::cleanPath(root).count('/');
	QDirIterator it(root, {"*.exe"}, QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext())
	{
		const QFileInfo f(it.next());
		const QString dir = QDir::cleanPath(f.absolutePath());
		if (dir.count('/') - base_depth > 4 || !is_game_exe(f))
			continue;
		const QString lower = dir.toLower();
		if (lower.contains("/_commonredist") || lower.contains("/redist") || lower.contains("/thirdparty") || lower.contains("/engine/extras"))
			continue;
		out.insert(dir);
	}
	return out.values();
}

QString game_root(const game_entry & g)
{
	return QDir::cleanPath(QDir::fromNativeSeparators(g.install_dir));
}
} // namespace

QString game_key(const game_entry & g)
{
	return QString("%1:%2").arg(g.platform == game_platform::oculus_rift ? "oculus" : "steam").arg(qulonglong(g.app_id));
}

bool wanted(const game_entry & g, bool by_default, const QMap<QString, bool> & per_game)
{
	return per_game.value(game_key(g), by_default);
}

QString bundled_dir()
{
	const QString dir = od::resource_dir("dxvk", "") + "/x64";
	for (const auto & d: dlls)
		if (!QFileInfo::exists(dir + "/" + d))
			return {};
	return dir;
}

bool installed(const game_entry & g)
{
	const QString root = game_root(g);
	if (root.isEmpty() || !QFileInfo::exists(root))
		return false;
	QDirIterator it(root, {marker}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
	return it.hasNext();
}

void apply(const game_entry & g, bool enable, const std::function<void(QString)> & log)
{
	const QString root = game_root(g);
	if (root.isEmpty() || !QFileInfo::exists(root))
		return;
	if (!enable)
	{
		QDirIterator it(root, {marker}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
		while (it.hasNext())
		{
			const QString manifest = it.next();
			const QString dir = QFileInfo(manifest).absolutePath();
			QFile m(manifest);
			if (!m.open(QIODevice::ReadOnly))
				continue;
			bool all_removed = true;
			for (const QByteArray & line: m.readAll().split('\n'))
			{
				const auto parts = line.trimmed().split(' '); // "<sha256> <file>"
				if (parts.size() != 2)
					continue;
				const QString file = dir + "/" + QString::fromUtf8(parts[1]);
				if (QFileInfo::exists(file) && digest(file) == parts[0])
					all_removed &= QFile::remove(file); // fails while the game runs: tried again next time
				else if (QFileInfo::exists(file))
					log(QString("[dxvk] %1: %2 changed since OpenDesk placed it, left alone").arg(g.name, file));
			}
			m.close();
			if (all_removed)
				QFile::remove(manifest);
			log(QString("[dxvk] %1: DXVK removed from %2").arg(g.name, QDir::toNativeSeparators(dir)));
		}
		return;
	}
	const QString source = bundled_dir();
	if (source.isEmpty())
	{
		log("[dxvk] DXVK is not installed with the streamer (reinstall OpenDesk)");
		return;
	}
	for (const QString & dir: exe_dirs(root))
	{
		if (QFileInfo::exists(dir + "/" + marker))
			continue; // already ours
		bool foreign = false;
		for (const auto & d: dlls)
			foreign |= QFileInfo::exists(dir + "/" + d);
		if (foreign)
		{
			log(QString("[dxvk] %1: %2 already has its own d3d11.dll / dxgi.dll, skipped").arg(g.name, QDir::toNativeSeparators(dir)));
			continue;
		}
		QByteArray manifest;
		bool ok = true;
		for (const auto & d: dlls)
		{
			ok &= QFile::copy(source + "/" + d, dir + "/" + d);
			manifest += digest(dir + "/" + d) + " " + d.toUtf8() + "\n";
		}
		QFile m(dir + "/" + marker);
		if (!ok) // all or nothing: no half-installed DXVK
			for (const auto & d: dlls)
				if (digest(dir + "/" + d) == digest(source + "/" + d))
					QFile::remove(dir + "/" + d);
		if (ok && m.open(QIODevice::WriteOnly))
		{
			m.write(manifest);
			m.close();
			SetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(m.fileName()).utf16()), FILE_ATTRIBUTE_HIDDEN);
			log(QString("[dxvk] %1: DXVK placed in %2").arg(g.name, QDir::toNativeSeparators(dir)));
		}
		else
			log(QString("[dxvk] %1: cannot write to %2 (run OpenDesk as administrator for games under Program Files)")
			            .arg(g.name, QDir::toNativeSeparators(dir)));
	}
}
} // namespace od::dxvk
#endif
