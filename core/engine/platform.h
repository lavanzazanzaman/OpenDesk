// Small platform helpers shared by the engine: process enumeration, process termination, the Steam
// installation root, and an OS description. Linux and Windows implementations live in platform.cpp.
#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace od::platform
{
struct process_entry
{
	int pid = 0;
	int ppid = 0;
	qulonglong start = 0; // creation token: safe against pid reuse
	QString exe;          // full path when available
	QStringList args;     // Linux: command line; Windows: empty
};

// One pass over the process list (Linux: /proc, Windows: Toolhelp).
std::vector<process_entry> processes();

// A process whose executable path or command line contains `needle` (case-insensitive on Windows).
bool any_process_has(const QString & needle);

bool terminate_process(int pid, bool force);

// Steam's installation root (empty when Steam is not installed).
QString steam_root();

// Human-readable OS description for the About tab.
QString os_description();
} // namespace od::platform
