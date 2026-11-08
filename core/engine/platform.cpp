#include "platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

#include <algorithm>

#include <csignal>
#include <unistd.h>

namespace od::platform
{
namespace
{
using process_entry = od::platform::process_entry;

QStringList split_args(const QByteArray & cmdline)
{
	QStringList out;
	for (const QByteArray & a: cmdline.split('\0'))
		if (!a.isEmpty())
			out << QString::fromLocal8Bit(a);
	return out;
}
} // namespace

std::vector<process_entry> processes()
{
	std::vector<process_entry> out;
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
		qsizetype close = s.lastIndexOf(')');
		QList<QByteArray> fields = s.mid(close + 2).split(' ');
		process_entry p;
		p.pid = pid;
		p.ppid = fields.size() > 1 ? fields[1].toInt() : 0;
		p.start = fields.size() > 19 ? fields[19].toULongLong() : 0;
		p.args = split_args(cmd.readAll());
		p.exe = p.args.value(0);
		out.push_back(std::move(p));
	}
	return out;
}

bool any_process_has(const QString & needle)
{
	for (const auto & p: processes())
	{
		if (p.exe.contains(needle))
			return true;
		for (const QString & a: p.args)
			if (a.contains(needle))
				return true;
	}
	return false;
}

bool terminate_process(int pid, bool force)
{
	return ::kill(pid, force ? SIGKILL : SIGTERM) == 0;
}

QString steam_root()
{
	QString data_home = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
	QString home = QDir::homePath();
	for (const QString & c: {data_home + "/Steam", home + "/.steam/steam",
	                         home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
	                         home + "/.var/app/com.valvesoftware.Steam/data/Steam"})
		if (QFileInfo::exists(c + "/steamapps/libraryfolders.vdf"))
			return QFileInfo(c).canonicalFilePath();
	return {};
}

QString os_description()
{
	return QString("%1 (Linux %2)").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
}
} // namespace od::platform
