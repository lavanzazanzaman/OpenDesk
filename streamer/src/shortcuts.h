// BINDINGS tab -> global shortcuts through xdg-desktop-portal GlobalShortcuts (Wayland apps can't grab keys
// themselves). The desktop (KDE: kglobalaccel) owns the actual key assignment; ours is the preferred trigger.
#pragma once

#include <QObject>
#include <QVariantMap>

#include <QDBusObjectPath>

class GlobalShortcuts : public QObject
{
	Q_OBJECT

public:
	explicit GlobalShortcuts(QObject * parent = nullptr);
	void bind(); // (re)register every binding that has a key; creates the session on first use
	bool available() const;

signals:
	void activated(QString action); // BindingAction name, e.g. "ToggleVRPassthrough"

private slots:
	void onResponse(uint response, const QVariantMap & results);
	void onActivated(const QDBusObjectPath & session, const QString & id, qulonglong timestamp, const QVariantMap & options);

private:
	void request(const QString & method, const QList<QVariant> & args);
	QString sessionHandle_;
	QString requestPath_;
	int counter_ = 0;
	bool bindPending_ = false;
};
