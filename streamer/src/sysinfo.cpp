#include "sysinfo.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>

namespace
{
QString readFile(const QString & path)
{
	QFile f(path);
	return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}
} // namespace

QString sysinfo::graphicsCards()
{
	// `lspci -mm`: "03:00.0" "VGA compatible controller" "Advanced Micro Devices, Inc. [AMD/ATI]" "Navi 48 [Radeon RX 9070/9070 XT/9070 GRE]" ...
	QProcess p;
	p.start("lspci", {"-mm"});
	if (!p.waitForFinished(2000))
		return "Unknown";
	QStringList cards;
	static const QRegularExpression field(R"re("([^"]*)")re");
	for (const QString & line: QString::fromUtf8(p.readAllStandardOutput()).split('\n'))
	{
		QStringList f;
		for (auto it = field.globalMatch(line); it.hasNext();)
			f << it.next().captured(1);
		if (f.size() >= 3 && (f[0].startsWith("VGA") || f[0].startsWith("3D") || f[0].startsWith("Display")))
		{
			QString vendor = f[1].contains("NVIDIA") ? "NVIDIA" : f[1].contains("AMD") ? "AMD" : f[1].contains("Intel") ? "Intel" : f[1];
			QString model = f[2];
			static const QRegularExpression bracket(R"(\[([^\]]+)\])");
			if (auto m = bracket.match(model); m.hasMatch())
				model = m.captured(1);
			cards << vendor + " " + model;
		}
	}
	return cards.isEmpty() ? "Unknown" : cards.join("\n");
}

QString sysinfo::processor()
{
	static const QRegularExpression re(R"(model name\s*:\s*(.+))");
	auto m = re.match(readFile("/proc/cpuinfo"));
	return m.hasMatch() ? m.captured(1).trimmed() : QSysInfo::currentCpuArchitecture();
}

QString sysinfo::memory()
{
	static const QRegularExpression re(R"(MemTotal:\s*(\d+) kB)");
	auto m = re.match(readFile("/proc/meminfo"));
	if (!m.hasMatch())
		return "Unknown";
	return QString("%1 GB").arg(qRound(m.captured(1).toDouble() / 1024 / 1024));
}

QString sysinfo::operatingSystem()
{
	return QSysInfo::prettyProductName() + " (Linux " + QSysInfo::kernelVersion() + ")";
}

QString sysinfo::secureBoot()
{
	// efivar SecureBoot-8be4df61-...: 4 attribute bytes + 1 value byte
	QDir vars("/sys/firmware/efi/efivars");
	if (!vars.exists())
		return "Not UEFI";
	for (const QString & name: vars.entryList({"SecureBoot-*"}, QDir::Files))
	{
		QFile f(vars.filePath(name));
		if (f.open(QIODevice::ReadOnly))
		{
			QByteArray d = f.readAll();
			if (d.size() >= 5)
				return d[4] ? "Enabled" : "Disabled";
		}
	}
	return "Unknown";
}

QString sysinfo::routerUrl()
{
	// /proc/net/route: default route has Destination 00000000, Gateway little-endian hex
	for (const QString & line: readFile("/proc/net/route").split('\n').mid(1))
	{
		QStringList f = line.split('\t', Qt::SkipEmptyParts);
		if (f.size() > 2 && f[1] == "00000000")
		{
			bool ok = false;
			uint32_t gw = f[2].toUInt(&ok, 16);
			if (ok && gw)
				return QString("http://%1.%2.%3.%4").arg(gw & 0xff).arg((gw >> 8) & 0xff).arg((gw >> 16) & 0xff).arg(gw >> 24);
		}
	}
	return {};
}

bool sysinfo::hasHardwareEncoder()
{
	return !QDir("/dev/dri").entryList({"renderD*"}, QDir::System).isEmpty();
}
