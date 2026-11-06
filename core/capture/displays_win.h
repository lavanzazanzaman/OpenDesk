// Windows display enumeration and capture setup (DXGI Desktop Duplication).
// Virtual monitors ("Add monitor", Extra display) come from the open-source Virtual Display Driver (an indirect display
// driver, https://github.com/VirtualDrivers/Virtual-Display-Driver) when it is installed: its monitor count is changed
// over its control pipe, and put back to what it was when the session ends.
#pragma once

#include "desktop_video.h"
#include "display_info.h"

#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class QTimer;

namespace od
{
class display_manager : public QObject
{
	Q_OBJECT

public:
	explicit display_manager(QObject * parent = nullptr);
	~display_manager() override;

	bool is_kwin() const { return false; }
	bool is_x11() const { return false; }
	QString kwin_unavailable_reason() const { return {}; }
	void start(const QString & restore_token, std::function<void(QString error)> done);
	bool ready() const { return started; }

	std::vector<display_info> displays();
	bool can_create_virtual() const; // the Virtual Display Driver answers on its pipe
	int virtual_count() const { return int(added.size()); }
	struct virtual_mode
	{
		int width = 1920, height = 1080;
		double scale = 1;
		int refresh_mhz = 60000;
	};
	virtual_mode main_monitor_mode();
	// asks the driver for one more monitor and returns at once with "" (pending): virtual_ready + displays_changed follow
	// when Windows has it (the driver restarts, which takes a few seconds)
	std::optional<std::string> add_virtual(virtual_mode, QString * error);
	bool remove_virtual(); // the newest one
	bool set_portrait(const display_info &, bool portrait, QString * error);

	std::optional<desktop_stream_setup> open(const display_info &, QString * error);
	void release_unused(const std::set<std::string> &) {}

signals:
	void displays_changed();
	void virtual_ready(QString name); // a monitor add_virtual asked for has appeared
	void restore_token(QString token);
	void log(QString line);

private:
	bool started = false;
	std::vector<std::string> added; // virtual monitors this session added (device names), oldest first
	int base_count = -1;            // the driver's monitor count before this session changed it
	QTimer * pending = nullptr; // waiting for the driver's new monitor
	std::vector<std::string> pending_before;
	virtual_mode pending_mode;
	int pending_polls = 0;
	void poll_pending();
};
} // namespace od
