// Linux equivalents of the Streamer's ABOUT tab fields.
#pragma once

#include <QString>

namespace sysinfo
{
QString graphicsCards();
QString processor();
QString memory();
QString operatingSystem();
QString secureBoot();
QString routerUrl();
bool hasHardwareEncoder();
} // namespace sysinfo
