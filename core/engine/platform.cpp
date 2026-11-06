#include "platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h> // UNICODE_STRING for NtQueryInformationProcess
#else
#include <csignal>
#include <unistd.h>
#endif

namespace od::platform
{
#ifdef _WIN32
namespace
{
qulonglong filetime_to_u64(const FILETIME & t)
{
	return (qulonglong(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

QString process_image(DWORD pid)
{
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h)
		return {};
	wchar_t buf[32768];
	DWORD size = DWORD(std::size(buf));
	QString out;
	if (QueryFullProcessImageNameW(h, 0, buf, &size))
		out = QString::fromWCharArray(buf, int(size));
	CloseHandle(h);
	return out;
}

qulonglong process_start(DWORD pid)
{
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h)
		return 0;
	FILETIME created{}, exited{}, kernel{}, user{};
	qulonglong start = 0;
	if (GetProcessTimes(h, &created, &exited, &kernel, &user))
		start = filetime_to_u64(created);
	CloseHandle(h);
	return start;
}

QStringList split_args(const QString & cmdline)
{
	// Good enough for "does this command line mention X" checks: split on spaces, keep quoted runs together.
	QStringList out;
	QString cur;
	bool quoted = false;
	for (QChar c: cmdline)
	{
		if (c == '"')
			quoted = !quoted;
		else if (c.isSpace() && !quoted)
		{
			if (!cur.isEmpty())
				out << cur, cur.clear();
		}
		else
			cur += c;
	}
	if (!cur.isEmpty())
		out << cur;
	return out;
}

// The full command line through NtQueryInformationProcess (no WMI, no child process).
QString process_command_line(DWORD pid)
{
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h)
		return {};
	using nt_query = LONG(WINAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	auto query = ntdll ? reinterpret_cast<nt_query>(GetProcAddress(ntdll, "NtQueryInformationProcess")) : nullptr;
	QString out;
	if (query)
	{
		ULONG needed = 0;
		query(h, 60 /* ProcessCommandLineInformation */, nullptr, 0, &needed);
		if (needed > sizeof(UNICODE_STRING))
		{
			std::vector<uint8_t> buf(needed + 2);
			if (query(h, 60, buf.data(), needed, &needed) == 0)
			{
				auto * us = reinterpret_cast<UNICODE_STRING *>(buf.data());
				out = QString::fromWCharArray(us->Buffer, us->Length / int(sizeof(wchar_t)));
			}
		}
	}
	CloseHandle(h);
	return out;
}
} // namespace

std::vector<process_entry> processes()
{
	std::vector<process_entry> out;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return out;
	PROCESSENTRY32W pe{};
	pe.dwSize = sizeof(pe);
	if (Process32FirstW(snap, &pe))
		do
		{
			const DWORD pid = pe.th32ProcessID;
			if (!pid || pid == GetCurrentProcessId())
				continue;
			process_entry p;
			p.pid = int(pid);
			p.ppid = int(pe.th32ParentProcessID);
			p.start = process_start(pid);
			p.exe = process_image(pid);
			p.args = split_args(process_command_line(pid));
			out.push_back(std::move(p));
		} while (Process32NextW(snap, &pe));
	CloseHandle(snap);
	return out;
}

bool any_process_has(const QString & needle)
{
	const QString wanted = needle.toLower();
	for (const auto & p: processes())
	{
		if (p.exe.toLower().contains(wanted))
			return true;
		for (const QString & a: p.args)
			if (a.toLower().contains(wanted))
				return true;
	}
	return false;
}

bool terminate_process(int pid, bool force)
{
	HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
	if (!h)
		return false;
	bool ok = TerminateProcess(h, force ? 1u : 0u) != FALSE;
	CloseHandle(h);
	return ok;
}

QString steam_root()
{
	QSettings settings("HKEY_CURRENT_USER\\Software\\Valve\\Steam", QSettings::NativeFormat);
	QString path = settings.value("SteamPath").toString();
	if (path.isEmpty())
	{
		QSettings machine("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam", QSettings::NativeFormat);
		path = machine.value("InstallPath").toString();
	}
	return QDir::toNativeSeparators(path);
}

QString os_description()
{
	return QString("%1 (Windows %2)").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
}
#else
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
#endif
} // namespace od::platform
