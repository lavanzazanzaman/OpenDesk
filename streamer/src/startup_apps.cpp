#include "startup_apps.h"

#include "engine.h"
#include "settings.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <csignal>
#include <unistd.h>

namespace
{
// companion apps we know: how to find them, and what they need to run alongside a VR game
struct KnownApp
{
	const char * name;
	QStringList programs;      // names on the PATH
	const char * file_pattern; // file names in the AppImage folders and among Flatpak's launchers
	const char * arguments;
};

const QList<KnownApp> & knownApps()
{
	static const QList<KnownApp> apps{
	        {"WayVR", {"wayvr", "wlx-overlay-s"}, "^(wayvr|wlx-overlay-s|.*\\.wayvr)([-_.].*)?(\\.appimage)?$", "--openxr"},
	        {"VRCX", {"vrcx"}, "^vrcx([-_.].*)?\\.appimage$", ""},
	};
	return apps;
}

QString quoted(const QString & path)
{
	if (!path.contains(QRegularExpression("[^A-Za-z0-9_./+=:@%-]")))
		return path;
	QString escaped = path;
	escaped.replace('\\', "\\\\").replace('"', "\\\"").replace('$', "\\$").replace('`', "\\`");
	return '"' + escaped + '"';
}

// the first word of a command, without its quotes
QString programOf(const QString & command)
{
	const QString c = command.trimmed();
	if (c.startsWith('"'))
	{
		QString out;
		for (int i = 1; i < c.size() && c[i] != '"'; ++i)
		{
			if (c[i] == '\\' && i + 1 < c.size())
				++i;
			out += c[i];
		}
		return out;
	}
	return c.section(' ', 0, 0);
}

const KnownApp * knownFor(const QString & fileName)
{
	for (const KnownApp & app: knownApps())
		if (app.programs.contains(fileName) ||
		    QRegularExpression(app.file_pattern, QRegularExpression::CaseInsensitiveOption).match(fileName).hasMatch())
			return &app;
	return nullptr;
}

// the apps' own environment, not the AppImage's library setup
QProcessEnvironment appEnvironment()
{
	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	for (const char * name: {"QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "APPDIR", "APPIMAGE", "ARGV0", "OWD"})
		env.remove(name);
	return env;
}
} // namespace

QString startupCommandFor(const QString & file)
{
	QString command = quoted(file);
	if (const KnownApp * app = knownFor(QFileInfo(file).fileName()); app && *app->arguments)
		command += QString(" ") + app->arguments;
	return command;
}

QString startupAppName(const QString & command)
{
	const QString file = QFileInfo(programOf(command)).fileName();
	if (const KnownApp * app = knownFor(file))
		return app->name;
	return file.isEmpty() ? command : file;
}

QList<DetectedApp> detectStartupApps()
{
	const QString home = QDir::homePath();
	const QStringList folders{home + "/.local/bin", home + "/bin",           home + "/Applications", home + "/AppImages",
	                          home + "/.local/share/AppImage", home + "/Downloads", home + "/Desktop", "/opt",
	                          home + "/.local/share/flatpak/exports/bin", "/var/lib/flatpak/exports/bin"};
	QList<DetectedApp> found;
	for (const KnownApp & app: knownApps())
	{
		QString path;
		for (const QString & program: app.programs)
			if (path.isEmpty())
				path = QStandardPaths::findExecutable(program);
		if (path.isEmpty()) // the newest copy
		{
			QRegularExpression pattern(app.file_pattern, QRegularExpression::CaseInsensitiveOption);
			QDateTime newest;
			for (const QString & folder: folders)
				for (const QFileInfo & file: QDir(folder).entryInfoList(QDir::Files | QDir::Hidden))
					if (pattern.match(file.fileName()).hasMatch() && (newest.isNull() || file.lastModified() > newest))
					{
						newest = file.lastModified();
						path = file.absoluteFilePath();
					}
		}
		if (!path.isEmpty())
			found.append({app.name, startupCommandFor(path)});
	}
	return found;
}

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
