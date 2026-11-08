#include "shortcuts.h"
#include "settings.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QKeySequence>

namespace
{
constexpr const char * service = "org.freedesktop.portal.Desktop";
constexpr const char * object = "/org/freedesktop/portal/desktop";
constexpr const char * iface = "org.freedesktop.portal.GlobalShortcuts";

// QKeySequence "Ctrl+Shift+F1" -> XDG shortcut trigger "CTRL+SHIFT+F1"
QString xdgTrigger(const QString & portable)
{
	QStringList parts = QKeySequence(portable).toString(QKeySequence::PortableText).split('+');
	for (QString & p: parts)
	{
		if (p == "Ctrl")
			p = "CTRL";
		else if (p == "Shift")
			p = "SHIFT";
		else if (p == "Alt")
			p = "ALT";
		else if (p == "Meta")
			p = "LOGO";
	}
	return parts.join('+');
}

const QMap<QString, QString> & descriptions()
{
	static const QMap<QString, QString> d{
	        {"SwitchMonitor", "OpenDesk: switch monitor"},
	        {"ToggleVRMode", "OpenDesk: toggle VR mode"},
	        {"ToggleVRPassthrough", "OpenDesk: toggle VR passthrough"},
	        {"EnableVRPassthrough", "OpenDesk: enable VR passthrough"},
	        {"DisableVRPassthrough", "OpenDesk: disable VR passthrough"},
	        {"ToggleHandPassthrough", "OpenDesk: toggle hand passthrough"},
	        {"ToggleDeskPassthrough", "OpenDesk: toggle desk passthrough"},
	        {"TogglePerformanceOverlay", "OpenDesk: toggle performance overlay"},
	        {"ToggleFoveatedStreaming", "OpenDesk: toggle foveated streaming"},
	};
	return d;
}

// a(sa{sv}) for BindShortcuts
struct Shortcut
{
	QString id;
	QVariantMap props;
};
QDBusArgument & operator<<(QDBusArgument & a, const Shortcut & s)
{
	a.beginStructure();
	a << s.id << s.props;
	a.endStructure();
	return a;
}
const QDBusArgument & operator>>(const QDBusArgument & a, Shortcut & s)
{
	a.beginStructure();
	a >> s.id >> s.props;
	a.endStructure();
	return a;
}
} // namespace
Q_DECLARE_METATYPE(Shortcut)

GlobalShortcuts::GlobalShortcuts(QObject * parent) :
        QObject(parent)
{
	qDBusRegisterMetaType<Shortcut>();
	qDBusRegisterMetaType<QList<Shortcut>>();
	QDBusConnection::sessionBus().connect(service, object, iface, "Activated", this,
	                                      SLOT(onActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
}

bool GlobalShortcuts::available() const
{
	QDBusMessage m = QDBusMessage::createMethodCall(service, object, "org.freedesktop.DBus.Properties", "Get");
	m.setArguments({iface, "version"});
	return QDBusConnection::sessionBus().call(m).type() == QDBusMessage::ReplyMessage;
}

void GlobalShortcuts::request(const QString & method, const QList<QVariant> & args)
{
	QString token = QString("vd_sc%1").arg(++counter_);
	QString sender = QDBusConnection::sessionBus().baseService().mid(1).replace('.', '_');
	requestPath_ = QString("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
	QDBusConnection::sessionBus().connect(service, requestPath_, "org.freedesktop.portal.Request", "Response", this,
	                                      SLOT(onResponse(uint, QVariantMap)));
	QList<QVariant> callArgs = args;
	QVariantMap options = callArgs.takeLast().toMap();
	options["handle_token"] = token;
	callArgs << options;
	QDBusMessage m = QDBusMessage::createMethodCall(service, object, iface, method);
	m.setArguments(callArgs);
	QDBusConnection::sessionBus().call(m, QDBus::NoBlock);
}

void GlobalShortcuts::bind()
{
	if (sessionHandle_.isEmpty())
	{
		bindPending_ = true;
		if (counter_ == 0) // create the session once
			request("CreateSession", {QVariantMap{{"session_handle_token", "vd_shortcuts"}}});
		return;
	}
	QList<Shortcut> list;
	const auto & b = StreamerSettings::instance().bindings;
	for (auto it = descriptions().begin(); it != descriptions().end(); ++it)
	{
		QVariantMap props{{"description", it.value()}};
		if (QString key = b.value(it.key()); !key.isEmpty())
			props["preferred_trigger"] = xdgTrigger(key);
		list << Shortcut{it.key(), props};
	}
	request("BindShortcuts", {QVariant::fromValue(QDBusObjectPath(sessionHandle_)), QVariant::fromValue(list), QString(), QVariantMap{}});
}

void GlobalShortcuts::onResponse(uint response, const QVariantMap & results)
{
	QDBusConnection::sessionBus().disconnect(service, requestPath_, "org.freedesktop.portal.Request", "Response", this,
	                                         SLOT(onResponse(uint, QVariantMap)));
	if (response != 0)
		return;
	if (sessionHandle_.isEmpty() && results.contains("session_handle"))
	{
		sessionHandle_ = results.value("session_handle").toString();
		if (std::exchange(bindPending_, false))
			bind();
	}
}

void GlobalShortcuts::onActivated(const QDBusObjectPath & session, const QString & id, qulonglong, const QVariantMap &)
{
	if (session.path() == sessionHandle_ && descriptions().contains(id))
		emit activated(id);
}

