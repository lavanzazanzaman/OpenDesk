// xdg-desktop-portal ScreenCast session (org.freedesktop.portal.ScreenCast, v4+ for restore tokens).
//   CreateSession -> SelectSources(monitor, embedded cursor, persist_mode=2, restore_token) -> Start
//   -> OpenPipeWireRemote: a PipeWire fd for the shared monitors' nodes.
// The first run shows the desktop's "share screen" dialog (several monitors may be picked for multi-monitor);
// the returned restore token is saved so later sessions start silently until the user revokes it.
#pragma once

#include <QDBusObjectPath>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <vector>

namespace od
{
struct portal_stream
{
	uint32_t node_id = 0;
	int x = 0, y = 0, width = 0, height = 0; // logical desktop coordinates (position may be unknown: 0,0)
	bool has_position = false;
	uint32_t source_type = 1; // 1 monitor, 2 window, 4 virtual
};

class screencast_portal : public QObject
{
	Q_OBJECT

public:
	explicit screencast_portal(QObject * parent = nullptr);
	~screencast_portal() override;

	// asynchronous; exactly one of the callbacks fires. Must run on a thread with a Qt event loop.
	void start(const QString & restore_token, bool multiple,
	           std::function<void(std::vector<portal_stream>, QString new_restore_token)> on_ready,
	           std::function<void(QString error)> on_error);
	// a fresh PipeWire connection fd for the session's nodes (one per capture stream); -1 on failure
	int open_remote(QString * error = nullptr);
	void close();

private slots:
	void on_response(uint response, const QVariantMap & results);

private:
	enum class step
	{
		idle,
		create_session,
		select_sources,
		start,
	};
	void request(const QString & method, const QList<QVariant> & args, step next);
	void fail(const QString & error);

	step current = step::idle;
	QString session_handle;
	QString restore_token;
	QString request_path;
	int token_counter = 0;
	bool multiple = false;
	std::function<void(std::vector<portal_stream>, QString)> ready_cb;
	std::function<void(QString)> error_cb;
};
} // namespace od
