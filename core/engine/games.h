// Games tab (Windows: GameManager). The headset shows two grids, "Oculus" and "Steam", with thumbnails only (no
// names), so the art has to carry the title.
//   RequestGames(u8 force)                  -> Games: i16 count, {u8 platform, u64 app id, bool running}
//   RequestGameThumbnails(i16 n, {u8, u64}) -> GameThumbnail(u8 platform, u64 app id, i32 size, PNG/JPG) per game
//   LaunchGame(u8, u64) / ExitGame(u8, u64); GameExited(u8, u64) when a running game's processes are gone
//
// Linux:   Oculus = RiftLift's library (~/.local/share/riftlift/games/*.json, launched with `riftlift launch
//          <slug>`); Steam = installed apps Steam flags as VR in appinfo.vdf.
// Windows: Oculus = the Oculus (Meta) library manifests under Oculus' "Software" folders, launched like the
//          original Windows streamer (OculusClient + the game executable); Steam = the same appinfo.vdf scan,
//          launched through the Steam client.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace od
{
enum class game_platform : uint8_t
{
	oculus_rift = 0,
	steam = 1,
	viveport = 2,
};
using game_id = std::pair<game_platform, uint64_t>;

struct game_entry
{
	game_platform platform = game_platform::steam;
	uint64_t app_id = 0;
	QString name;
	QString riftlift_slug;         // Linux Oculus: `riftlift launch` argument
	QString riftlift_key;          // Linux Oculus: RiftLiftLauncher.exe /app <key>
	QString launch_exe;            // Windows: executable to start directly (Oculus games)
	QString launch_args;           // Windows: Oculus manifest launchParameters
	QString install_dir;           // Windows: library folder, used to find the running game
	std::vector<QString> art;      // wide banner art with the title, best first
	QString hero, logo;            // fallback: background + title logo
	game_id id() const { return {platform, app_id}; }
};

std::vector<game_entry> scan_games(); // Oculus library first, then Steam VR apps; blocking (reads appinfo.vdf)

// thumbnail for the headset's 220x128 tile: the game's wide art scaled down, or a generated card with its name
QByteArray game_thumbnail(const game_entry &);

// one pass over the process list; a game counts as running while one of its processes exists
class process_table
{
public:
	process_table();
	// Linux RiftLift: RIFTLIFT_LAUNCH_ID of each launch whose processes match the game's command line. Callers keep
	// them: once the launcher exits, only the marker still ties Wine's reparented processes to it. Unused elsewhere.
	using markers = std::set<QByteArray>;
	markers launch_markers(const game_entry &) const;
	bool running(const game_entry &, const markers & known = {}) const;
	// the game's processes: matched by command line / executable, plus all their descendants
	std::vector<int> pids(const game_entry &, const markers & known = {}) const;
	std::vector<int> launchers(const game_entry &) const; // Linux: `riftlift launch <slug>`
	// exit: the game's processes now (pid + start time, safe against pid reuse and a relaunch of the same game), and
	// later what of them is left plus anything carrying those launches' markers
	using snapshot = std::vector<std::pair<int, qulonglong>>;
	snapshot take(const game_entry &, const markers &) const;
	std::vector<int> remaining(const snapshot &, const markers &) const;

private:
	struct process
	{
		int pid = 0, ppid = 0;
		QString exe;
		QStringList args;
		qulonglong start = 0; // stat starttime / Windows creation time
		QByteArray riftlift_launch, steam_app_id; // RIFTLIFT_LAUNCH_ID / SteamAppId from the environment
	};
	std::vector<int> roots(const game_entry &, const markers & known) const;
	std::vector<process> procs;
};

QString riftlift_executable();        // Linux: empty if RiftLift is not installed
QString oculus_client_executable();   // Windows: OculusClient.exe, empty if Oculus is not installed
} // namespace od
