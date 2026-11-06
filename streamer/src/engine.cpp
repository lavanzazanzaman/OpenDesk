#include "engine.h"
#include "settings.h"
#include "sysinfo.h"

#include "native_engine.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QScreen>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTimer>

Engine::Engine(QObject * parent) :
        QObject(parent)
{
	QDir().mkpath(QFileInfo(logPath()).path());
	log_.setFileName(logPath());
	if (!log_.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		qWarning("cannot open log %s", qPrintable(logPath()));

	connect(&proc_, &QProcess::readyReadStandardOutput, this, &Engine::onStdout);
	connect(&proc_, &QProcess::readyReadStandardError, this, &Engine::onStderr);
	connect(&proc_, &QProcess::started, this, [this] {
		setStatus(Status::Listening);
		sendAccounts(StreamerSettings::instance().allAccountIDs());
	});
	connect(&proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
		setStatus(Status::Stopped);
		log_.write(QString("--- engine exited (%1)\n").arg(code).toUtf8());
		log_.flush();
		if (!stopping_) // crashed: restart like a service would
			QTimer::singleShot(2000, this, &Engine::start);
	});
}

Engine::~Engine()
{
	stop();
}

QString Engine::logPath() const
{
	return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation))
	        .filePath("OpenDesk/streamer.log");
}

QStringList Engine::arguments() const
{
	const auto & s = StreamerSettings::instance();
	QStringList args{"-u", QDir(OPENDESK_ENGINE_DIR).filePath("opendesk_streamer.py"), "--json"};
	// desktop capture codec: the engine can do H.264 and HEVC so far
	if (s.preferredCodec == VideoCodec::HEVC || s.preferredCodec == VideoCodec::HEVC10bit)
		args << "--codec" << "hevc";
	if (s.audioStreaming == AudioStreaming::Disabled)
		args << "--no-audio";
	args << "--vr";
	for (const QString & id: s.allAccountIDs()) // also pushed on stdin whenever they change
		args << "--account" << id;
	return args;
}

// connected displays; Qt substitutes a nameless placeholder screen when every output is gone
static int realScreens()
{
	int n = 0;
	for (QScreen * sc: QGuiApplication::screens())
		n += !sc->name().isEmpty();
	return n;
}

QJsonObject Engine::optionsJson() const
{
	const auto & s = StreamerSettings::instance();
	const bool hevc = s.preferredCodec == VideoCodec::HEVC || s.preferredCodec == VideoCodec::HEVC10bit;
	QScreen * screen = QGuiApplication::primaryScreen();
	QSize desktop = screen ? screen->size() * screen->devicePixelRatio() : QSize();
	const QString gpu = sysinfo::graphicsCards().section('\n', 0, 0);
	return {{"codec", hevc ? "hevc" : "h264"}, // Python engine (VR test grid)
	        {"codec_id", int(s.preferredCodec)},
	        {"auto_adjust_bitrate", s.autoAdjustBitrate},
	        {"aq", s.enableAQ},
	        {"two_pass", s.enable2Pass},
	        {"adaptive_speed", s.adaptiveEncoderSpeed},
	        {"fovea_size", s.foveaSize},
	        {"periphery_scale", s.peripheryScale},
	        {"audio_mode", int(s.audioStreaming)},
	        {"gamepad_emulation", int(s.gamepadEmulation)},
	        {"virtual_audio", s.useVirtualAudioDriver},
	        {"auto_select_mic", s.autoSelectMicrophone},
	        {"encrypt_local", s.encryptLocalTraffic},
	        {"lock_on_disconnect", s.lockComputer},
	        {"screenshots_dir", s.screenshotsRootPath},
	        {"videos_root", s.videosRootPath},
	        {"adapter_name", gpu},
	        {"adapter_is_amd", gpu.startsWith("AMD")},
	        {"no_audio", s.audioStreaming == AudioStreaming::Disabled},
	        {"horizontal_fov", s.horizontalFovTangent},
	        {"vertical_fov", s.verticalFovTangent},
	        {"resolution_scale", s.renderResolutionOverride},
	        {"sharpening", s.useSharpening ? s.sharpening : 0.0},
	        {"use_stencil", s.useFovStencil},
	        {"soft_fovea_edge", s.softFoveaEdge},
	        {"boost_priority", s.boostGamePriority},
	        {"touch", s.useTouchInput},
	        {"mouth_sensitivity", s.mouthSensitivity},
	        {"reported_version", s.reportedStreamerVersion},
	        {"screens", realScreens()},
	        {"desktop_width", desktop.width()},
	        {"desktop_height", desktop.height()}};
}

// Native C++ engine; the old Python engine only with OPENDESK_ENGINE=python
static bool wantNative()
{
	return qEnvironmentVariable("OPENDESK_ENGINE") != "python";
}

void Engine::start()
{
	if (wantNative())
	{
		if (native_)
			return;
		native_ = new od::native_engine;
		native_->moveToThread(&thread_);
		connect(native_, &od::native_engine::event, this, &Engine::handle);
		connect(native_, &od::native_engine::log, this, [this](const QString & line) {
			log_.write(line.toUtf8() + "\n");
			log_.flush();
		});
		thread_.setObjectName("vd-engine");
		thread_.start();
		log_.write("--- starting native engine\n");
		QMetaObject::invokeMethod(native_, "start", Qt::QueuedConnection,
		                          Q_ARG(QStringList, StreamerSettings::instance().allAccountIDs()),
		                          Q_ARG(QJsonObject, optionsJson()));
		setStatus(Status::Listening);
		return;
	}
	if (proc_.state() != QProcess::NotRunning)
		return;
	stopping_ = false;
	QString venv = QDir(OPENDESK_ENGINE_DIR).filePath(".venv/bin/python");
	QString python = QFileInfo::exists(venv) ? venv : "python3";
	log_.write(QString("--- starting %1 %2\n").arg(python, arguments().join(' ')).toUtf8());
	proc_.start(python, arguments());
}

void Engine::stop()
{
	if (native_)
	{
		// delete the engine on its own thread (its sessions join the audio/video/capture threads),
		// then stop the thread; a deleteLater after the loop ended would never run
		od::native_engine * n = std::exchange(native_, nullptr);
		QMetaObject::invokeMethod(n, [n] { delete n; }, Qt::BlockingQueuedConnection);
		thread_.quit();
		if (!thread_.wait(5000))
			log_.write("--- engine thread did not stop within 5 s\n");
		setStatus(Status::Stopped);
		return;
	}
	if (proc_.state() == QProcess::NotRunning)
		return;
	stopping_ = true;
	proc_.write(R"({"cmd":"quit"})" "\n");
	if (!proc_.waitForFinished(2000))
	{
		proc_.terminate();
		if (!proc_.waitForFinished(2000))
			proc_.kill();
	}
}

void Engine::restart()
{
	stop();
	start();
}

void Engine::sendAccounts(const QStringList & accounts)
{
	if (native_)
		return (void)QMetaObject::invokeMethod(native_, "set_accounts", Qt::QueuedConnection, Q_ARG(QStringList, accounts));
	if (proc_.state() != QProcess::Running)
		return;
	QJsonObject cmd{{"cmd", "set_accounts"}, {"accounts", QJsonArray::fromStringList(accounts)}};
	proc_.write(QJsonDocument(cmd).toJson(QJsonDocument::Compact) + "\n");
}

void Engine::applyOptions()
{
	if (bool(native_) != wantNative() && (native_ || proc_.state() != QProcess::NotRunning))
		return restart(); // switching backends (VR test grid needs the Python engine)
	if (native_)
		return (void)QMetaObject::invokeMethod(native_, "set_options", Qt::QueuedConnection, Q_ARG(QJsonObject, optionsJson()));
	if (proc_.state() != QProcess::Running)
		return start();
	QJsonObject cmd = optionsJson();
	cmd["cmd"] = "set_options";
	proc_.write(QJsonDocument(cmd).toJson(QJsonDocument::Compact) + "\n");
}

void Engine::headsetAction(const QString & action)
{
	if (native_)
		QMetaObject::invokeMethod(native_, "headset_action", Qt::QueuedConnection, Q_ARG(QString, action));
}

void Engine::onStderr()
{
	log_.write(proc_.readAllStandardError());
	log_.flush();
}

void Engine::onStdout()
{
	stdoutBuffer_ += proc_.readAllStandardOutput();
	qsizetype nl;
	while ((nl = stdoutBuffer_.indexOf('\n')) >= 0)
	{
		QByteArray line = stdoutBuffer_.left(nl);
		stdoutBuffer_.remove(0, nl + 1);
		QJsonParseError err;
		auto doc = QJsonDocument::fromJson(line, &err);
		if (err.error == QJsonParseError::NoError && doc.isObject())
			handle(doc.object());
		else
			log_.write("stdout: " + line + "\n");
	}
}

void Engine::handle(const QJsonObject & ev)
{
	const QString type = ev["event"].toString();
	if (type == "ready")
	{
		computer = ev;
		emit ready();
	}
	else if (type == "pairing_request")
		emit pairingRequest(ev["account"].toString(), ev["platform"].toInt(), ev["host"].toString());
	else if (type == "connected")
	{
		connectedViaUsb = ev["usb"].toBool();
		setStatus(Status::Connected);
	}
	else if (type == "disconnected")
	{
		captureErrorMessage.clear();
		headsetMobileSettings = {};
		headsetUserSettings = {};
		lastVideo = {};
		setStatus(native_ || proc_.state() == QProcess::Running ? Status::Listening : Status::Stopped);
		emit headsetChanged();
		emit videoChanged();
	}
	else if (type == "settings")
	{
		const QString kind = ev["kind"].toString();
		QJsonObject & target = kind.startsWith("Mobile") ? headsetMobileSettings : headsetUserSettings;
		if (kind.endsWith("Reload"))
			target = {};
		const QJsonObject values = ev["values"].toObject();
		for (auto it = values.begin(); it != values.end(); ++it)
			target[it.key()] = it.value();
		emit headsetChanged();
	}
	else if (type == "video")
	{
		lastVideo = ev;
		captureErrorMessage.clear();
		emit videoChanged();
	}
	else if (type == "screenshot")
		emit screenshotSaved(ev["path"].toString());
	else if (type == "headset_newer")
		emit headsetNewer(ev["supported"].toString());
	else if (type == "capture_error")
	{
		captureErrorMessage = ev["message"].toString();
		emit captureError(captureErrorMessage);
		emit videoChanged();
	}
}

void Engine::setStatus(Status s)
{
	if (s == status_)
		return;
	status_ = s;
	emit statusChanged(s);
}
