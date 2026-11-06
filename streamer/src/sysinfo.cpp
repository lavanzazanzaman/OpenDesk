#include "sysinfo.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <dxgi.h>
#include <iphlpapi.h>

#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

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
#ifdef _WIN32
	IDXGIFactory * factory = nullptr;
	if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void **>(&factory))) || !factory)
		return "Unknown";
	QStringList cards;
	for (UINT i = 0;; ++i)
	{
		IDXGIAdapter * adapter = nullptr;
		if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC desc{};
		if (SUCCEEDED(adapter->GetDesc(&desc)))
			cards << QString::fromWCharArray(desc.Description).trimmed();
		adapter->Release();
	}
	factory->Release();
	cards.removeAll(QString());
	return cards.isEmpty() ? "Unknown" : cards.join("\n");
#else
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
#endif
}

QString sysinfo::processor()
{
#ifdef _WIN32
	QSettings cpu("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", QSettings::NativeFormat);
	const QString name = cpu.value("ProcessorNameString").toString().trimmed();
	return name.isEmpty() ? QSysInfo::currentCpuArchitecture() : name;
#else
	static const QRegularExpression re(R"(model name\s*:\s*(.+))");
	auto m = re.match(readFile("/proc/cpuinfo"));
	return m.hasMatch() ? m.captured(1).trimmed() : QSysInfo::currentCpuArchitecture();
#endif
}

QString sysinfo::memory()
{
#ifdef _WIN32
	MEMORYSTATUSEX status{};
	status.dwLength = sizeof(status);
	if (!GlobalMemoryStatusEx(&status))
		return "Unknown";
	return QString("%1 GB").arg(qRound64(double(status.ullTotalPhys) / 1024 / 1024 / 1024));
#else
	static const QRegularExpression re(R"(MemTotal:\s*(\d+) kB)");
	auto m = re.match(readFile("/proc/meminfo"));
	if (!m.hasMatch())
		return "Unknown";
	return QString("%1 GB").arg(qRound(m.captured(1).toDouble() / 1024 / 1024));
#endif
}

QString sysinfo::operatingSystem()
{
#ifdef _WIN32
	return QSysInfo::prettyProductName() + " (Windows " + QSysInfo::kernelVersion() + ")";
#else
	return QSysInfo::prettyProductName() + " (Linux " + QSysInfo::kernelVersion() + ")";
#endif
}

QString sysinfo::secureBoot()
{
#ifdef _WIN32
	QSettings state("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", QSettings::NativeFormat);
	const QVariant value = state.value("UEFISecureBootEnabled");
	if (!value.isValid())
		return "Not UEFI";
	return value.toInt() ? "Enabled" : "Disabled";
#else
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
#endif
}

QString sysinfo::routerUrl()
{
#ifdef _WIN32
	ULONG size = 15000;
	std::vector<uint8_t> buffer(size);
	auto * adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
	ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, adapters, &size);
	if (result == ERROR_BUFFER_OVERFLOW)
	{
		buffer.resize(size);
		adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
		result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, adapters, &size);
	}
	if (result != NO_ERROR)
		return {};
	for (auto * a = adapters; a; a = a->Next)
		for (auto * g = a->FirstGatewayAddress; g; g = g->Next)
			if (g->Address.lpSockaddr && g->Address.lpSockaddr->sa_family == AF_INET)
			{
				auto * in = reinterpret_cast<sockaddr_in *>(g->Address.lpSockaddr);
				const uint8_t * b = reinterpret_cast<const uint8_t *>(&in->sin_addr);
				if (b[0] || b[1] || b[2] || b[3])
					return QString("http://%1.%2.%3.%4").arg(b[0]).arg(b[1]).arg(b[2]).arg(b[3]);
			}
	return {};
#else
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
#endif
}

bool sysinfo::hasHardwareEncoder()
{
#ifdef _WIN32
	for (const char * name: {"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"})
		if (avcodec_find_encoder_by_name(name))
			return true;
	return false;
#else
	return !QDir("/dev/dri").entryList({"renderD*"}, QDir::System).isEmpty();
#endif
}
