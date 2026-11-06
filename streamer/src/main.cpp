#include "engine.h"
#include "mainwindow.h"
#include "settings.h"
#include "setup.h"

#include <QApplication>
#include <QFile>
#include <QLockFile>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include <exception>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <QSocketNotifier>
#include <csignal>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
int crash_fd = -1; // streamer.log

void crash_write(const char * text)
{
	if (crash_fd >= 0)
#ifdef _WIN32
		[[maybe_unused]] int r = _write(crash_fd, text, int(strlen(text)));
#else
		[[maybe_unused]] ssize_t r = write(crash_fd, text, strlen(text));
#endif
#ifdef _WIN32
	OutputDebugStringA(text);
#else
	[[maybe_unused]] ssize_t e = write(STDERR_FILENO, text, strlen(text));
#endif
}

#ifndef _WIN32
// a crash (often on a friend's machine) leaves its stack in streamer.log: each frame as module + offset, which
// addr2line -e <module> resolves against the same build
void crash_handler(int sig)
{
	char line[512];
	snprintf(line, sizeof(line), "--- crash: signal %d (%s)\n", sig, strsignal(sig));
	crash_write(line);
	void * frames[64];
	int n = backtrace(frames, 64);
	for (int i = 0; i < n; ++i)
	{
		Dl_info info{};
		if (dladdr(frames[i], &info) && info.dli_fname)
			snprintf(line, sizeof(line), "  #%d %s+0x%zx %s\n", i, info.dli_fname, size_t((char *)frames[i] - (char *)info.dli_fbase),
			         info.dli_sname ? info.dli_sname : "");
		else
			snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
		crash_write(line);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

#endif

void install_crash_handler(int log_fd)
{
	crash_fd = log_fd;
#ifndef _WIN32
	void * warm[1];
	backtrace(warm, 1); // loads libgcc now, not inside the handler
	struct sigaction sa{};
	sa.sa_handler = crash_handler;
	sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	for (int sig: {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL})
		sigaction(sig, &sa, nullptr);
#endif
	std::set_terminate([] {
		if (auto e = std::current_exception())
			try
			{
				std::rethrow_exception(e);
			}
			catch (const std::exception & ex)
			{
				crash_write("--- uncaught exception: ");
				crash_write(ex.what());
				crash_write("\n");
			}
			catch (...)
			{
				crash_write("--- uncaught exception\n");
			}
		abort();
	});
}
} // namespace

int main(int argc, char ** argv)
{
#ifndef _WIN32
	// The ELF loader has already cached its search path. Restore the user's
	// environment for Steam/Proton and helpers spawned later by this process.
	if (qEnvironmentVariableIsSet("OPENDESK_APPIMAGE_LD_LIBRARY_PATH")) {
		const auto original = qgetenv("OPENDESK_APPIMAGE_LD_LIBRARY_PATH");
		if (original.isEmpty()) qunsetenv("LD_LIBRARY_PATH");
		else qputenv("LD_LIBRARY_PATH", original);
		qunsetenv("OPENDESK_APPIMAGE_LD_LIBRARY_PATH");
	}
#endif
	QApplication app(argc, argv);
	QApplication::setApplicationName("OpenDesk Streamer");
	QApplication::setDesktopFileName("opendesk-streamer");
	QApplication::setQuitOnLastWindowClosed(false); // lives in the tray

	if (QApplication::arguments().contains("--system-check"))
	{
		// headless: the System check dialog's checks; --fix also runs the fixes that need no root
		bool fix = QApplication::arguments().contains("--fix");
		int problems = 0;
		for (SetupItem & item: runSetupChecks())
		{
			QString result;
			if (!item.ok && fix && item.userFix)
			{
				QString error = item.userFix();
				result = error.isEmpty() ? " -> fixed" : " -> fix failed: " + error;
				problems += !error.isEmpty();
			}
			else
				problems += !item.ok;
			printf("%s %s: %s%s\n", item.ok ? "ok  " : "FAIL", qPrintable(item.title), qPrintable(item.detail), qPrintable(result));
		}
		return problems ? 1 : 0;
	}

	const bool screenshot = QApplication::arguments().contains("--screenshot");
	QLockFile lock(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/opendesk-streamer.lock");
	if (!screenshot && !lock.tryLock(100))
	{
		qWarning("OpenDesk Streamer is already running");
		return 1;
	}

	QFile qss(":/theme/violet.qss");
	if (qss.open(QIODevice::ReadOnly))
		app.setStyleSheet(QString::fromUtf8(qss.readAll()));

	Engine engine;
	install_crash_handler(engine.logFd());
	MainWindow window(engine);

	// dev aid: `--screenshot DIR` renders every tab to DIR/tab-N.png without starting the engine
	if (int i = QApplication::arguments().indexOf("--screenshot"); i > 0 && i + 1 < QApplication::arguments().size())
		return window.screenshotTabs(QApplication::arguments()[i + 1]) ? 0 : 1;
	QObject::connect(&app, &QApplication::aboutToQuit, &engine, &Engine::stop);

	engine.start();
	// test hook: quit like the tray's Exit after N seconds
	bool quitAfterSet = false;
	int quitAfter = qEnvironmentVariableIntValue("OPENDESK_QUIT_AFTER", &quitAfterSet);
	if (quitAfterSet)
		QTimer::singleShot(quitAfter * 1000, &app, [] {
			qInfo("OPENDESK_QUIT_AFTER: quitting");
			QApplication::quit();
		});
	if (!StreamerSettings::instance().startMinimizedInTray)
		window.show();
	if (StreamerSettings::instance().checkSetupAtStartup)
	{
		SetupDialog check(window.isVisible() ? &window : nullptr);
		if (check.hasProblems())
			check.exec();
	}
#ifndef _WIN32
	// SIGTERM/SIGINT/SIGHUP (logout, shutdown, kill): quit normally so the engine undoes its system changes
	// (OpenXR runtime, audio routing, virtual monitors). Signal handler -> pipe -> Qt event loop.
	static int signal_pipe[2];
	if (pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK) == 0)
	{
		struct sigaction sa{};
		sa.sa_handler = [](int) { [[maybe_unused]] ssize_t r = write(signal_pipe[1], "q", 1); };
		sigemptyset(&sa.sa_mask);
		for (int sig: {SIGTERM, SIGINT, SIGHUP})
			sigaction(sig, &sa, nullptr);
		auto * notifier = new QSocketNotifier(signal_pipe[0], QSocketNotifier::Read, &app);
		QObject::connect(notifier, &QSocketNotifier::activated, &app, [] {
			char c;
			while (read(signal_pipe[0], &c, 1) > 0)
			{
			}
			qInfo("signal received: quitting");
			QCoreApplication::quit();
		});
	}
#endif
	return app.exec();
}
