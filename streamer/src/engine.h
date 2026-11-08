// Runs the streaming engine and bridges its events. Two backends:
//   default           C++ engine (core/engine) on a thread in this process
//   OPENDESK_ENGINE=python  engine/opendesk_streamer.py as a subprocess (also used for the VR test grid), JSON line protocol:
//   engine -> GUI (stdout): {"event": "ready"|"pairing_request"|"selected"|"connected"|"disconnected"|"settings"|"video", ...}
//   GUI -> engine (stdin):  {"cmd": "set_accounts", "accounts": [...]} | {"cmd": "quit"}
#pragma once

#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QThread>

namespace od
{
class native_engine;
}

class Engine : public QObject
{
	Q_OBJECT

public:
	enum class Status
	{
		Stopped,
		Listening, // "connected to server" in the Windows tray icon terms
		Connected, // "connected to peer"
	};

	explicit Engine(QObject * parent = nullptr);
	~Engine() override;

	void start();
	void stop();
	void restart();
	void sendAccounts(const QStringList & accounts);
	void applyOptions();
	void applyGpuSelection(); // live: codec / audio / VR test mode, no reconnect
	void headsetAction(const QString & action); // BINDINGS -> headset

	Status status() const { return status_; }
	QString logPath() const;
	int logFd() const { return log_.handle(); } // crash reports (main.cpp)

	// latest headset state (merged settings sync)
	QJsonObject headsetMobileSettings;
	QJsonObject headsetUserSettings;
	QJsonObject lastVideo;
	QJsonObject lastEncodeStats;    // VR: ms, worst, fps, megapixels, nvenc
	qint64 lastEncodeStatsAt = 0;  // when (ms since epoch)
	QString captureErrorMessage; // screen capture failed for the current connection
	QJsonObject computer; // "ready" event
	bool connectedViaUsb = false;

signals:
	void statusChanged(Engine::Status);
	void pairingRequest(QString account, int platform, QString host);
	void headsetChanged();
	void videoChanged();
	void encodeStatsChanged();
	void captureError(QString message);
	void screenshotSaved(QString path);
	void headsetNewer(QString supported); // the headset app is newer than this streamer's upstream version
	void ready();

private:
	void onStdout();
	void onStderr();
	void handle(const QJsonObject & ev);
	void setStatus(Status);
	QStringList arguments() const;

	bool native() const { return native_ != nullptr; }
	QJsonObject optionsJson() const;

	QProcess proc_;
	QThread thread_;                      // native engine (OPENDESK_ENGINE=native)
	od::native_engine * native_ = nullptr;
	QFile log_;
	QByteArray stdoutBuffer_;
	Status status_ = Status::Stopped;
	bool stopping_ = false;
};
