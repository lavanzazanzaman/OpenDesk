// Small platform helpers shared by the engine: process enumeration, process termination, the Steam
// installation root, and an OS description. The implementation lives in platform.cpp.
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
	QStringList args;     // command line
};

// One pass over the process list (/proc).
std::vector<process_entry> processes();

// A process whose executable path or command line contains `needle`.
bool any_process_has(const QString & needle);

bool terminate_process(int pid, bool force);

// Steam's installation root (empty when Steam is not installed).
QString steam_root();

// Human-readable OS description for the About tab.
QString os_description();
} // namespace od::platform
