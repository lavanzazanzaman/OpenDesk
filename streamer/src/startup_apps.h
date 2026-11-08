// The apps from General > Startup apps: started when a VR game opens, closed when it has gone.
#pragma once

#include <QObject>
#include <QTimer>

#include <vector>

class Engine;
class QProcess;

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
