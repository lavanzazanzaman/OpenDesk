#include "startup_apps.h"

#include "engine.h"
#include "settings.h"

#include <QDir>
#include <QProcess>

#include <csignal>
#include <unistd.h>

namespace
{
// the apps' own environment, not the AppImage's library setup
QProcessEnvironment appEnvironment()
{
	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	for (const char * name: {"QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "APPDIR", "APPIMAGE", "ARGV0", "OWD"})
		env.remove(name);
	return env;
}
} // namespace

StartupApps::StartupApps(Engine & engine, QObject * parent) :
        QObject(parent), engine_(engine)
{
	grace_.setSingleShot(true);
	grace_.setInterval(8000);
	connect(&grace_, &QTimer::timeout, this, &StartupApps::stop);
	connect(&engine_, &Engine::vrGameChanged, this, &StartupApps::gameChanged);
}

StartupApps::~StartupApps()
{
	for (QProcess * p: running_)
	{
		const qint64 group = p->processId();
		if (group > 0)
			::kill(-group, SIGTERM);
		p->waitForFinished(1500);
		if (group > 0) // (whatever it started and left behind too)
			::kill(-group, SIGKILL);
	}
}

void StartupApps::gameChanged(bool running)
{
	if (running)
	{
		grace_.stop();
		if (running_.empty())
			start();
	}
	else if (!running_.empty() && !grace_.isActive())
		grace_.start();
}

void StartupApps::start()
{
	QStringList commands = StreamerSettings::instance().startupApps;
	if (const QString test = qEnvironmentVariable("OPENDESK_TEST_STARTUP_APP"); !test.isEmpty())
		commands << test;
	for (const QString & command: commands)
	{
		if (command.trimmed().isEmpty())
			continue;
		auto * p = new QProcess(this);
		p->setProgram("sh");
		p->setArguments({"-c", command});
		p->setProcessEnvironment(appEnvironment());
		p->setWorkingDirectory(QDir::homePath());
		p->setStandardOutputFile(QProcess::nullDevice());
		p->setStandardErrorFile(QProcess::nullDevice());
		p->setChildProcessModifier([] { ::setsid(); }); // its own process group: closing it reaches what it started
		connect(p, &QProcess::errorOccurred, this, [this, command](QProcess::ProcessError e) {
			if (e == QProcess::FailedToStart)
				engine_.note("[startup apps] could not start: " + command);
		});
		connect(p, &QProcess::finished, this, [this, p, command](int code) {
			engine_.note(QString("[startup apps] exited (%1): %2").arg(code).arg(command));
			std::erase(running_, p);
			p->deleteLater();
		});
		p->start();
		running_.push_back(p);
		engine_.note("[startup apps] started: " + command);
	}
}

void StartupApps::stop()
{
	for (QProcess * p: running_)
	{
		const qint64 group = p->processId();
		if (group <= 0)
			continue;
		::kill(-group, SIGTERM);
		// a few seconds to save and close; then the group goes, however it reacted
		QTimer::singleShot(3000, [group] { ::kill(-group, SIGKILL); });
	}
}
