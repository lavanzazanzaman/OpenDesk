// The monitors that can be streamed and how to capture them (Windows: DynamicSettings.Monitors + IVirtualMonitor).
//   KWin (screen cast protocol granted): every output, no dialog, plus virtual outputs for Add monitor / Extra
//            display. Streams are opened per output on demand.
//   Portal (any other desktop): the monitors the user picked in the share dialog (several allowed); the restore
//            token keeps the choice. No virtual outputs.
// Lives on the engine thread (Qt event loop), one per headset session: virtual outputs and temporary rotations are
// undone when it goes away.
#pragma once

#include "desktop_video.h"
#include "kwin_screencast.h"

#include <QObject>
#include <QString>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QSocketNotifier;

namespace od
{
class screencast_portal;

class display_manager : public QObject
{
	Q_OBJECT

public:
	explicit display_manager(QObject * parent = nullptr);
	~display_manager() override;

	bool is_kwin() const { return bool(kwin); }
	bool is_x11() const { return x11; }
	QString kwin_unavailable_reason() const { return kwin_reason; }

	// KWin: ready immediately. Portal: asks the desktop (dialog the first time), then calls back.
	// Calling it again restarts the portal session (e.g. after the shared monitor went away).
	void start(const QString & portal_restore_token, std::function<void(QString error)> done);
	bool ready() const { return started; }

	std::vector<display_info> displays();
	bool can_create_virtual() const { return bool(kwin); }
	int virtual_count() const { return int(virtuals.size()); }
	struct virtual_mode
	{
		int width = 1920, height = 1080; // logical
		double scale = 1;
		int refresh_mhz = 60000;
	};
	virtual_mode main_monitor_mode(); // the desktop's primary physical monitor
	// adds a virtual output (logical size, scale, refresh rate); returns its output name
	std::optional<std::string> add_virtual(virtual_mode, QString * error);
	bool remove_virtual(); // the newest one
	// portrait/landscape for one screen: virtual outputs are recreated with swapped sides, physical ones rotated
	// through kscreen-doctor until the session ends (Windows: SetMonitorPortrait(permanent: false))
	bool set_portrait(const display_info &, bool portrait, QString * error);

	// capture source for a display; KWin opens a fresh stream for physical outputs (closing an older one)
	std::optional<desktop_stream_setup> open(const display_info &, QString * error);
	void release_unused(const std::set<std::string> & in_use);

signals:
	void displays_changed();              // KWin outputs added/removed/rearranged
	void restore_token(QString token);    // portal: save for the next session
	void log(QString line);

private:
	std::unique_ptr<kwin_screencast> kwin;
	bool x11 = false; // X11 session: XRandR monitors, grabbed with MIT-SHM (x11_capture)
	QString kwin_reason;
	QSocketNotifier * notifier = nullptr;
	std::unique_ptr<screencast_portal> portal;
	std::vector<std::pair<display_info, uint32_t>> portal_displays; // + node id
	bool started = false;

	struct virtual_output
	{
		int handle = 0;
		int number = 0; // Upstream<n>: stable names, so KWin remembers placement and modes
		std::string name;
		stream_node node;
		virtual_mode mode;
	};
	std::optional<virtual_output> create_virtual(virtual_mode, QString * error);
	void apply_refresh(const std::string & output, int width, int height, int refresh_mhz);
	std::vector<virtual_output> virtuals;
	std::map<std::string, int> physical_streams; // output name -> kwin stream handle
	std::map<std::string, int> rotated;          // output name -> original wl_output transform
};
} // namespace od
