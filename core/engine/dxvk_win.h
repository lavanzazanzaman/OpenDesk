// Windows: DXVK per VR game. DXVK (Direct3D 9-11 on Vulkan, zlib license) works by d3d11.dll / dxgi.dll next to the
// game's executable; the streamer places them there for the games it is enabled for and takes them away again.
//   - DXVK itself ships with the streamer (<install>\dxvk\x64, tools/package-windows.sh)
//   - every directory under the game's folder holding a game executable gets the DLLs; existing d3d11.dll / dxgi.dll
//     that aren't ours are never touched (that directory is skipped and reported)
//   - .opendesk-dxvk in each directory lists what was placed (with sizes); only those files, still unchanged, are
//     removed again
// OpenDesk XR detects DXVK's device in the game and composites on DXVK's own Vulkan device (libovr-opendesk).
#pragma once

#include "games.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

namespace od::dxvk
{
QString game_key(const game_entry &); // "steam:<appid>" / "oculus:<id>" (settings key)
bool wanted(const game_entry &, bool by_default, const QMap<QString, bool> & per_game);
QString bundled_dir(); // the DXVK DLLs shipped with the streamer (x64), "" if missing
// places (enable) or removes (disable) DXVK for one game; log gets one line per directory changed or skipped
void apply(const game_entry &, bool enable, const std::function<void(QString)> & log);
bool installed(const game_entry &); // a .opendesk-dxvk is in the game's folder tree
} // namespace od::dxvk
