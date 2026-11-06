// Windows display enumeration and capture setup (DXGI Desktop Duplication).
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
	bool can_create_virtual() const { return false; }
	int virtual_count() const { return 0; }
	struct virtual_mode
	{
		int width = 1920, height = 1080;
		double scale = 1;
		int refresh_mhz = 60000;
	};
	virtual_mode main_monitor_mode();
	std::optional<std::string> add_virtual(virtual_mode, QString * error);
	bool remove_virtual() { return false; }
	bool set_portrait(const display_info &, bool portrait, QString * error);

	std::optional<desktop_stream_setup> open(const display_info &, QString * error);
	void release_unused(const std::set<std::string> &) {}

signals:
	void displays_changed();
	void restore_token(QString token);
	void log(QString line);

private:
	bool started = false;
};
} // namespace od
