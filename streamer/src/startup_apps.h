// The apps from General > Startup apps: started when a VR game opens, closed when it has gone.
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <vector>

class Engine;
class QProcess;

struct DetectedApp
{
	QString name, command;
};
// VR companion apps found on this PC (installed programs and AppImages in the usual folders)
QList<DetectedApp> detectStartupApps();
// the command for a program or AppImage the user picked (with the arguments it needs, if it's a known app)
QString startupCommandFor(const QString & file);
// what to call a command in the list: a known app's name, else the program's file name
QString startupAppName(const QString & command);

class StartupApps : public QObject
{
	Q_OBJECT
public:
	explicit StartupApps(Engine & engine, QObject * parent = nullptr);
	~StartupApps() override;

private:
	void gameChanged(bool running);
	void start();
	void stop();

	Engine & engine_;
	QTimer grace_; // a game restarting right away leaves the apps running
	std::vector<QProcess *> running_;
};
