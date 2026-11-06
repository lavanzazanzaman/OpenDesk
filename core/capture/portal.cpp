#include "portal.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>

#include <unistd.h>

namespace od
{
namespace
{
constexpr const char * service = "org.freedesktop.portal.Desktop";
constexpr const char * object = "/org/freedesktop/portal/desktop";
constexpr const char * screencast = "org.freedesktop.portal.ScreenCast";

constexpr uint source_monitor = 1;
constexpr uint cursor_embedded = 2;
constexpr uint persist_until_revoked = 2;
} // namespace

screencast_portal::screencast_portal(QObject * parent) :
        QObject(parent) {}

screencast_portal::~screencast_portal()
{
	close();
}

void screencast_portal::start(const QString & token, bool multi, std::function<void(std::vector<portal_stream>, QString)> on_ready,
                              std::function<void(QString)> on_error)
{
	restore_token = token;
	multiple = multi;
	ready_cb = std::move(on_ready);
	error_cb = std::move(on_error);
	QVariantMap options{{"session_handle_token", QString("vd_session%1").arg(++token_counter)}};
	request("CreateSession", {options}, step::create_session);
}

void screencast_portal::close()
{
	if (session_handle.isEmpty())
		return;
	QDBusMessage msg = QDBusMessage::createMethodCall(service, session_handle, "org.freedesktop.portal.Session", "Close");
	QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
	session_handle.clear();
}

int screencast_portal::open_remote(QString * error)
{
	QDBusMessage msg = QDBusMessage::createMethodCall(service, object, screencast, "OpenPipeWireRemote");
	msg.setArguments({QVariant::fromValue(QDBusObjectPath(session_handle)), QVariantMap{}});
	QDBusReply<QDBusUnixFileDescriptor> fd = QDBusConnection::sessionBus().call(msg);
	if (!fd.isValid())
	{
		if (error)
			*error = "OpenPipeWireRemote: " + fd.error().message();
		return -1;
	}
	return dup(fd.value().fileDescriptor()); // QDBusUnixFileDescriptor closes its own copy
}

void screencast_portal::request(const QString & method, const QList<QVariant> & args, step next)
{
	// Subscribe to the Request's Response *before* calling, using the path the portal will create:
	// /org/freedesktop/portal/desktop/request/<sender without ':' and '.'->'_'>/<handle_token>
	QString handle_token = QString("vd_req%1").arg(++token_counter);
	QString sender = QDBusConnection::sessionBus().baseService().mid(1).replace('.', '_');
	request_path = QString("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, handle_token);
	QDBusConnection::sessionBus().connect(service, request_path, "org.freedesktop.portal.Request", "Response", this,
	                                      SLOT(on_response(uint, QVariantMap)));

	QList<QVariant> call_args = args;
	QVariantMap options = call_args.takeLast().toMap();
	options["handle_token"] = handle_token;
	call_args << options;

	QDBusMessage msg = QDBusMessage::createMethodCall(service, object, screencast, method);
	msg.setArguments(call_args);
	current = next;
	QDBusMessage reply = QDBusConnection::sessionBus().call(msg);
	if (reply.type() == QDBusMessage::ErrorMessage)
		fail(QString("%1: %2").arg(method, reply.errorMessage()));
}

void screencast_portal::fail(const QString & error)
{
	current = step::idle;
	if (error_cb)
		error_cb(error);
}

void screencast_portal::on_response(uint response, const QVariantMap & results)
{
	QDBusConnection::sessionBus().disconnect(service, request_path, "org.freedesktop.portal.Request", "Response", this,
	                                         SLOT(on_response(uint, QVariantMap)));
	if (response != 0)
		return fail(response == 1 ? "screen sharing was cancelled" : "screen sharing request failed");

	switch (current)
	{
		case step::create_session:
		{
			session_handle = results.value("session_handle").toString();
			QVariantMap options{{"types", source_monitor}, {"multiple", multiple}, {"cursor_mode", cursor_embedded},
			                    {"persist_mode", persist_until_revoked}};
			if (!restore_token.isEmpty())
				options["restore_token"] = restore_token;
			request("SelectSources", {QVariant::fromValue(QDBusObjectPath(session_handle)), options}, step::select_sources);
			break;
		}
		case step::select_sources:
			request("Start", {QVariant::fromValue(QDBusObjectPath(session_handle)), QString(), QVariantMap{}}, step::start);
			break;
		case step::start:
		{
			current = step::idle;
			std::vector<portal_stream> list;
			// streams: a(ua{sv}) - node id + properties ("position": (ii), "size": (ii), "source_type": u)
			auto pair = [](const QVariant & v, int & a, int & b) {
				const QDBusArgument arg = v.value<QDBusArgument>();
				arg.beginStructure();
				arg >> a >> b;
				arg.endStructure();
			};
			const QDBusArgument streams = results.value("streams").value<QDBusArgument>();
			streams.beginArray();
			while (!streams.atEnd())
			{
				portal_stream stream;
				QVariantMap props;
				streams.beginStructure();
				streams >> stream.node_id >> props;
				streams.endStructure();
				if (props.contains("size"))
					pair(props.value("size"), stream.width, stream.height);
				if (props.contains("position"))
				{
					pair(props.value("position"), stream.x, stream.y);
					stream.has_position = true;
				}
				stream.source_type = props.value("source_type", 1u).toUInt();
				if (stream.node_id)
					list.push_back(stream);
			}
			streams.endArray();
			if (list.empty())
				return fail("the portal returned no stream");
			if (ready_cb)
				ready_cb(list, results.value("restore_token").toString());
			break;
		}
		case step::idle:
			break;
	}
}
} // namespace od
