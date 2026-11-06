#include "app_paths.h"
#include "native_engine.h"
#include "adapters.h"
#include "platform.h"
#include "desktop_video.h"
#include "file_player.h"
#include "hardware_encoder.h"
#include "face_tracking.h"
#include "netbuf.h"
#include "video_library.h"
#include "video.h"
#include "runtime_registration.h"
#include "vr_sender.h"

#ifdef _WIN32
#include "audio_win.h"
#include "dxvk_win.h"
#include "gamepad_win.h"
#include "input_win.h"
#include "pulse_win.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#else
#include "audio.h"
#include "gamepad.h"
#include "input.h"
#include "pulse.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <csignal>
#include <signal.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <QDateTime>
#include <QDesktopServices>
#include <QThreadPool>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QFile>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <csignal>
#include <QUuid>

#include <lz4.h>
#include <climits>
#include <format>
#include <set>

namespace od
{
namespace
{
// Windows HmdTypeExtensions: how many monitors a headset may show at once
int max_monitors(int hmd_type)
{
	switch (hmd_type)
	{
		case 10: case 20: case 25: case 30: case 40: case 45: case 50: // phones, tablets, TVs
		case 259:                                                       // Oculus Quest
			return 1;
		case 1000: case 1001: case 1010: case 1100: case 1400: case 1500: case 1600: // Quest 3/3S, Pico 4 Ultra, ...
			return 3;
		case 1700: case 1710: // Pico Space Pro, Meta VR Glasses
			return 4;
		default:
			return 2;
	}
}
constexpr size_t frame_data_size = 4992; // FrameData, dcgen --layout

QString config_path()
{
	return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
	        .filePath("OpenDesk/engine.json");
}

// Computer ID shared with the Python engine so the headset sees the same PC
std::string load_computer_id()
{
	QFile f(config_path());
	QJsonObject o;
	if (f.open(QIODevice::ReadOnly))
		o = QJsonDocument::fromJson(f.readAll()).object();
	if (!o.contains("id"))
	{
		o["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
		QDir().mkpath(QFileInfo(config_path()).path());
		QFile w(config_path());
		if (w.open(QIODevice::WriteOnly | QIODevice::Truncate))
			w.write(QJsonDocument(o).toJson());
	}
	return o["id"].toString().toStdString();
}

QString host_of(const QHostAddress & a)
{
	QHostAddress bare = a;
	bare.setScopeId({});
	if (bool ok; bare.toIPv4Address(&ok), ok) // v4-mapped from the dual-stack TCP servers
		return QHostAddress(bare.toIPv4Address()).toString();
	return bare.toString();
}

QTcpServer * listen(QObject * parent, uint16_t port)
{
	auto * server = new QTcpServer(parent);
	if (!server->listen(QHostAddress::Any, port)) // dual-stack
		qWarning("cannot listen on tcp/%u: %s", port, qPrintable(server->errorString()));
	return server;
}

QUdpSocket * bind_udp(QObject * parent, const QHostAddress & any)
{
	auto * s = new QUdpSocket(parent);
	if (!s->bind(any, discovery_port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint))
		qWarning("cannot bind udp/%u: %s", discovery_port, qPrintable(s->errorString()));
	return s;
}
} // namespace

struct native_engine::session
{
	// sockets first: members are destroyed in reverse order, so the sender threads below stop before
	// anything could close (and recycle) the fds they write to
	QPointer<QTcpSocket> audio_socket;
	QPointer<QTcpSocket> video_socket;
	QPointer<QTcpSocket> messaging;
	bool encrypted = false; // EncryptLocalTraffic as advertised when this headset connected
	std::unique_ptr<aes_stream_reader> data_reader, audio_reader, video_reader;
	std::unique_ptr<aes_stream_writer> data_writer; // streamer -> headset on the data connection (one cipher stream)
	// Videos tab "Download": a PC video copied to the headset (NetDataClient.ReadVideoAsync, FullVideo)
	std::unique_ptr<QFile> download;
	qint64 download_left = 0;
	bytes video_in; // headset -> streamer on the video connection (eye gaze)
	bool gaze_seen = false;
	bool spacewarp_logged = false;
	bool newer_headset_reported = false; // UpdateRequired seen
	pulse_client * pulse = nullptr;
	// audio routing to undo when the session ends (or the mode changes)
	std::optional<uint32_t> audio_sink_module, mic_module;
	std::string restore_sink, unmute_sink, restore_source;
	std::unique_ptr<aes_stream_reader> reader;
	std::unique_ptr<aes_stream_writer> writer;
	QPointer<QTcpSocket> data;
	bytes data_buffer;
	bytes latest_frame_data;
	// VR: what the sender thread needs from the headset, updated on the engine thread
	struct tracking
	{
		std::mutex m;
		bytes frame;            // latest FrameData (HeadsetPose @120, FrameIndex @0)
		std::array<std::pair<int32_t, int64_t>, 128> arrivals{}; // FrameIndex -> steady ns it arrived (ring)
		float ipd = 0.063f;
		fov_port fov[2];
	};
	std::shared_ptr<tracking> vr_tracking = std::make_shared<tracking>();
	uint64_t frame_data_packets = 0;
	QJsonObject user, mobile, streamer;
	bytes mic_buffer;
	std::unique_ptr<input_injector> injector;
	std::unique_ptr<virtual_gamepad> gamepad;
	std::unique_ptr<mic_player> mic;
	std::unique_ptr<stream_channel> audio_out, video_out; // before their senders: destroyed after them
	std::unique_ptr<desktop_audio_sender> audio;
	bool video_ready = false; // bandwidth measured + grace period: headset is IsConnected
	desktop_video_params video_params;
	// monitors: declared before `video` so the capture streams stop before their sources are closed
	std::unique_ptr<display_manager> displays;
	bool displays_starting = false;
	std::vector<display_info> streamed;    // what the video currently shows, by stream index
	std::string layout_signature;          // displays the current layout was computed from
	int monitor_start = 0;                 // DynamicSettings.MonitorStartIndex
	bool has_switched_monitor = false;
	bool extra_display = false;            // a virtual output exists for SharedUserSettings.ExtraDisplay
	QJsonObject monitor_flags;             // Can{Switch,Add,Remove}Monitor last sent
	bool restart_pending = false;
	std::shared_ptr<video_source> video; // shared: the VR server may hold it as its frame sink (cleared first)
	std::string vr_params_signature;
	bool vr_external = false;
	std::unique_ptr<file_player> player; // Videos tab: a file instead of the desktop (StreamingSource.Video)
	QString playing_path;
	bool capture_failed = false;

	void undo_audio_output()
	{
		audio.reset(); // stop capturing before the sink goes away
		if (!pulse)
			return;
		if (!restore_sink.empty())
			pulse->set_default_sink(std::exchange(restore_sink, {}));
		if (!unmute_sink.empty())
			pulse->set_sink_mute(std::exchange(unmute_sink, {}), false);
		if (audio_sink_module)
			pulse->unload_module(*std::exchange(audio_sink_module, std::nullopt));
	}
	void undo_mic()
	{
		mic.reset();
		if (!pulse)
			return;
		if (!restore_source.empty())
			pulse->set_default_source(std::exchange(restore_source, {}));
		if (mic_module)
			pulse->unload_module(*std::exchange(mic_module, std::nullopt));
	}
	~session()
	{
		undo_audio_output();
		undo_mic();
	}
};

native_engine::native_engine(QObject * parent) :
        QObject(parent)
{
#ifndef _WIN32
	QDBusConnection::sessionBus().connect("org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "layoutChanged", this,
	                                      SLOT(on_keyboard_layout_changed(uint)));
	foveated_layer::remove_leftovers();
#endif
}

// SharedStreamerSettings.InputLanguage: Windows sends the layout's name; the headset picks its keyboard by it
// (assets/languageLayouts.json) and falls back to US
#ifdef _WIN32
QString native_engine::input_language() const
{
	// GetKeyboardLayout(0) is the thread's active layout. Map the common KLIDs; anything else falls back to US,
	// which is what the headset does for an unknown name too.
	const quint32 id = quint32(quintptr(GetKeyboardLayout(0))) & 0xffff;
	static const QMap<quint32, QString> names{
	        {0x0409, "US"},          {0x0809, "United Kingdom"}, {0x0c09, "US"},        {0x0407, "German"},
	        {0x0807, "Swiss German"}, {0x040c, "French"},        {0x0c0c, "Canadian French"},
	        {0x0410, "Italian"},      {0x041d, "Swedish"},       {0x0419, "Russian"},   {0x0415, "Polish"},
	        {0x041f, "Turkish Q"},    {0x0422, "Ukrainian"},     {0x040b, "Finnish"},   {0x0411, "Japanese"},
	        {0x0416, "Portuguese (Brazil ABNT2)"}, {0x0816, "Portuguese"}, {0x0c0a, "Spanish"}, {0x040a, "Spanish"},
	        {0x040e, "Hungarian"},    {0x0412, "Korean"},        {0x0804, "Chinese"},   {0x0404, "Chinese"},
	};
	return names.value(id, "US");
}
#else
QString native_engine::input_language() const
{
	QDBusMessage layout = QDBusConnection::sessionBus().call(
	        QDBusMessage::createMethodCall("org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "getLayout"), QDBus::Block, 500);
	QDBusMessage list = QDBusConnection::sessionBus().call(
	        QDBusMessage::createMethodCall("org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "getLayoutsList"), QDBus::Block,
	        500);
	if (layout.type() != QDBusMessage::ReplyMessage || list.type() != QDBusMessage::ReplyMessage || list.arguments().isEmpty())
		return "US";
	uint index = layout.arguments().value(0).toUInt();
	QString name, variant;
	const QDBusArgument arg = list.arguments()[0].value<QDBusArgument>();
	arg.beginArray();
	for (uint i = 0; !arg.atEnd(); ++i)
	{
		QString s, v, l;
		arg.beginStructure();
		arg >> s >> v >> l;
		arg.endStructure();
		if (i == index)
			name = s, variant = v;
	}
	arg.endArray();
	static const QMap<QString, QString> names{
	        {"us", "US"},           {"gb", "United Kingdom"}, {"ie", "Irish"},   {"de", "German"},     {"se", "Swedish"},
	        {"fr", "French"},       {"be", "Belgian French"}, {"it", "Italian"}, {"tr", "Turkish Q"},  {"ru", "Russian"},
	        {"br", "Portuguese (Brazil ABNT2)"}, {"pt", "Portuguese"}, {"es", "Spanish"}, {"latam", "Latin American"},
	        {"hu", "Hungarian"},    {"ua", "Ukrainian"},      {"fi", "Finnish"}, {"jp", "Japanese"},
	};
	if (name == "ch")
		return variant.startsWith("fr") ? "French Standard" : "Swiss German";
	if (name == "ca")
		return variant == "eng" ? "US" : "Canadian French";
	if (name == "fr" && variant.startsWith("latin9"))
		return "French Legacy";
	return names.value(name, "US");
}

void native_engine::on_keyboard_layout_changed(uint)
{
	if (current && current->messaging)
		send_streamer_settings({{"InputLanguage", input_language()}}, false);
}
#endif

native_engine::~native_engine()
{
	openxr_runtime::restore(); // hand the system its OpenXR runtime back (Windows: when that needs no UAC prompt)
	openvr_runtime::restore(); // ... and its OpenVR runtime
	if (vr)
		vr->set_sink(nullptr);
	if (current)
		stop_video_source(current.get());
}

std::optional<std::pair<aes_key, aes_iv>> native_engine::channel_encryption(session * sess) const
{
	if (!sess->encrypted)
		return std::nullopt;
	return std::pair{computer.key, computer.iv};
}

void native_engine::info(const QString & msg)
{
	emit log(QDateTime::currentDateTime().toString("HH:mm:ss") + " INFO " + msg);
}

void native_engine::emit_event(const char * name, QJsonObject fields)
{
	fields["event"] = name;
	emit event(fields);
}

void native_engine::start(QStringList accounts_, QJsonObject options_)
{
	set_accounts(accounts_);
	if (qEnvironmentVariableIsSet("OPENDESK_STREAMER_VERSION"))
		options.reported_version = parse_streamer_version(qgetenv("OPENDESK_STREAMER_VERSION").toStdString());
	set_options(options_);
	computer.version = options.reported_version;
	// face/eye tracking to VRChat: always on (sends only while VRChat runs and the headset has the data)
	face = std::make_unique<face_tracking>([this](const std::string & line) { info(QString::fromStdString(line)); });
	face->set_mouth_sensitivity(options.mouth_sensitivity);
	auto * face_timer = new QTimer(this);
	connect(face_timer, &QTimer::timeout, this, [this] { face->tick(); });
	face_timer->start(1000);

	computer.id = load_computer_id();
	{
		QFile f(config_path());
		if (f.open(QIODevice::ReadOnly))
		{
			QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
			portal_restore_token = o.value("portal_restore_token").toString();
			monitor_count = o.value("monitor_count").toInt(0);
		}
	}
	computer.name = QHostInfo::localHostName().toStdString();
	computer.description = platform::os_description().toStdString();
	computer.adapters = local_adapters();
	random_bytes(computer.connection_id.data(), computer.connection_id.size());
	computer.key = random_array<32>();
	computer.iv = random_array<16>();

	QJsonArray adapters;
	for (const auto & a: computer.adapters)
	{
		adapters.append(QJsonObject{{"ip", QString::fromStdString(a.ip)}, {"wifi", a.is_wireless}, {"gbit", a.is_gigabit}, {"usb", a.is_usb}});
		info(QString("  adapter %1 wifi=%2 gbit=%3 usb=%4").arg(QString::fromStdString(a.ip)).arg(a.is_wireless).arg(a.is_gigabit).arg(a.is_usb));
	}
	info(QString("computer id=%1 name='%2' (native engine)").arg(QString::fromStdString(computer.id), QString::fromStdString(computer.name)));

	discovery_v4 = bind_udp(this, QHostAddress::AnyIPv4);
	discovery_v6 = bind_udp(this, QHostAddress::AnyIPv6);
	for (auto * s: {discovery_v4, discovery_v6})
		connect(s, &QUdpSocket::readyRead, this, [this, s] { on_discovery(s); });

	messaging_server = listen(this, messaging_port);
	data_server = listen(this, data_port);
	video_server = listen(this, video_port);
	audio_server = listen(this, audio_port);
	connect(messaging_server, &QTcpServer::newConnection, this, &native_engine::on_messaging_connection);
	connect(data_server, &QTcpServer::newConnection, this, &native_engine::on_data_connection);
	connect(video_server, &QTcpServer::newConnection, this, &native_engine::on_video_connection);
	// capture ended (display switched off, GPU reset, sharing revoked...): restart it with the saved permission,
	// but not while there is no display at all, and with a back-off so a persistent failure can't loop fast
	auto * watchdog = new QTimer(this);
	connect(watchdog, &QTimer::timeout, this, [this] {
		if (!current || !current->video || current->video->running() || current->capture_failed || options.screens <= 0 ||
		    current->player)
			return;
		qint64 now = QDateTime::currentMSecsSinceEpoch();
		qint64 wait = quick_capture_restarts < 5 ? 2000 : 15000;
		if (now - last_capture_restart_ms < wait)
			return;
		quick_capture_restarts = now - last_capture_restart_ms < 60000 ? quick_capture_restarts + 1 : 0;
		last_capture_restart_ms = now;
		info("[capture] stream ended (display switched off?) - restarting capture");
		if (current->displays && !current->displays->is_kwin())
		{
			stop_video_source(current.get());
			current->displays.reset(); // a portal node that died stays dead: new portal session (silent via the token)
		}
		update_video(true);
	});
	watchdog->start(1000);
	connect(audio_server, &QTcpServer::newConnection, this, &native_engine::on_audio_connection);

	emit_event("ready", {{"id", QString::fromStdString(computer.id)},
	                     {"name", QString::fromStdString(computer.name)},
	                     {"version", QString::fromStdString(version_string(computer.version))},
	                     {"adapters", adapters}});
	info(QString("listening: udp/%1, tcp/%2-%3").arg(discovery_port).arg(messaging_port).arg(audio_port));
}

void native_engine::set_accounts(QStringList list)
{
	accounts = QSet<QString>(list.begin(), list.end());
	info("allowed accounts: " + list.join(", "));
}

void native_engine::set_options(QJsonObject o)
{
	if (o.contains("reported_version"))
	{
		options.reported_version = parse_streamer_version(o["reported_version"].toString().toStdString());
		computer.version = options.reported_version;
	}
	if (o.contains("codec_id"))
		options.codec_id = o["codec_id"].toInt();
	if (o.contains("auto_adjust_bitrate"))
		options.auto_adjust_bitrate = o["auto_adjust_bitrate"].toBool();
	if (o.contains("aq"))
		options.aq = o["aq"].toBool();
	if (o.contains("two_pass"))
		options.two_pass = o["two_pass"].toBool();
	if (o.contains("adaptive_speed"))
		options.adaptive_speed = o["adaptive_speed"].toBool();
	if (o.contains("periphery_scale"))
		options.periphery_scale = std::clamp(o["periphery_scale"].toDouble(0.5), 0.3, 0.5);
	if (o.contains("fovea_size"))
		options.fovea_size = std::clamp(o["fovea_size"].toDouble(0.25), 0.15, 0.5); // Windows: 15-35%, default 25%
	if (o.contains("adapter_name"))
		options.adapter_name = o["adapter_name"].toString();
	if (o.contains("adapter_is_amd"))
		options.adapter_is_amd = o["adapter_is_amd"].toBool();
	if (current && current->messaging && o.contains("auto_adjust_bitrate") &&
	    current->streamer.value("AutoAdjustBitrate").toBool() != options.auto_adjust_bitrate)
		send_streamer_settings({{"AutoAdjustBitrate", options.auto_adjust_bitrate}}, false);
	if (o.contains("no_audio"))
		options.no_audio = o["no_audio"].toBool();
	int audio_mode_before = options.audio_mode;
	bool virtual_before = options.virtual_audio;
	if (o.contains("audio_mode"))
		options.audio_mode = o["audio_mode"].toInt();
	if (o.contains("virtual_audio"))
		options.virtual_audio = o["virtual_audio"].toBool();
	if (o.contains("dxvk_default") || o.contains("dxvk_games"))
	{
		options.dxvk_default = o["dxvk_default"].toBool(options.dxvk_default);
		if (o.contains("dxvk_games"))
		{
			options.dxvk_games.clear();
			const QJsonObject games_o = o["dxvk_games"].toObject();
			for (auto it = games_o.begin(); it != games_o.end(); ++it)
				options.dxvk_games[it.key()] = it.value().toBool();
		}
		QMetaObject::invokeMethod(this, [this] { sync_dxvk(); }, Qt::QueuedConnection);
	}
	if (o.contains("boost_priority"))
	{
		options.boost_priority = o["boost_priority"].toBool();
		QMetaObject::invokeMethod(this, [this] { apply_game_priority(); }, Qt::QueuedConnection);
	}
	if (o.contains("auto_select_mic"))
		options.auto_select_mic = o["auto_select_mic"].toBool();
	if (o.contains("encrypt_local"))
		options.encrypt_local = o["encrypt_local"].toBool(); // applies from the next connection
	if (o.contains("lock_on_disconnect"))
		options.lock_on_disconnect = o["lock_on_disconnect"].toBool();
	if (o.contains("gamepad_emulation"))
		options.gamepad_emulation = o["gamepad_emulation"].toInt();
	if (o.contains("videos_root"))
		options.videos_root = o["videos_root"].toString();
	if (o.contains("screenshots_dir"))
		options.screenshots_dir = o["screenshots_dir"].toString();
	computer.encrypt_local = options.encrypt_local;
	audio_enabled = options.audio_mode != 0;
	if (current && current->audio_socket && (options.audio_mode != audio_mode_before || options.virtual_audio != virtual_before))
	{
		current->undo_audio_output();
		setup_audio_output(current.get());
	}
	if (o.contains("horizontal_fov")) options.render.horizontal_fov = float(std::clamp(o["horizontal_fov"].toDouble(1), .5, 1.));
	if (o.contains("vertical_fov")) options.render.vertical_fov = float(std::clamp(o["vertical_fov"].toDouble(1), .5, 1.));
	if (o.contains("resolution_scale")) options.render.resolution_scale = float(std::clamp(o["resolution_scale"].toDouble(1), .5, 2.));
	if (o.contains("sharpening")) options.render.sharpening = float(std::clamp(o["sharpening"].toDouble(), 0., 1.));
	if (o.contains("use_stencil")) options.render.use_stencil = o["use_stencil"].toBool(true);
	if (o.contains("soft_fovea_edge")) options.render.soft_fovea_edge = o["soft_fovea_edge"].toBool();
	QMetaObject::invokeMethod(this, [this] { update_vr_server(); }, Qt::QueuedConnection);
	if (o.contains("touch"))
		options.touch = o["touch"].toBool();
	if (o.contains("mouth_sensitivity"))
		options.mouth_sensitivity = float(o["mouth_sensitivity"].toDouble(1));
	if (face)
		face->set_mouth_sensitivity(options.mouth_sensitivity);
	if (o.contains("desktop_width"))
		options.desktop_width = o["desktop_width"].toInt();
	if (o.contains("desktop_height"))
		options.desktop_height = o["desktop_height"].toInt();
	if (o.contains("virtual_width") && o["virtual_width"].toInt() > 0)
		options.virtual_width = o["virtual_width"].toInt();
	if (o.contains("virtual_height") && o["virtual_height"].toInt() > 0)
		options.virtual_height = o["virtual_height"].toInt();
	if (o.contains("screens"))
	{
		int before = std::exchange(options.screens, o["screens"].toInt());
		// a display came back (or the layout changed): restart capture right away instead of waiting for the watchdog
		bool kwin_tracks = current && current->displays && current->displays->is_kwin(); // it has its own signal
		if (options.screens > 0 && options.screens != before && current && current->video_ready && !current->capture_failed && !kwin_tracks)
		{
			info(QString("[capture] displays changed (%1 -> %2) - restarting capture").arg(before).arg(options.screens));
			quick_capture_restarts = 0;
			last_capture_restart_ms = QDateTime::currentMSecsSinceEpoch();
			QTimer::singleShot(1500, this, [this] { update_video(true); }); // let the compositor settle
		}
	}
	if (current && current->injector)
		current->injector->set_touch_mode(options.touch);
	update_video();
}

void native_engine::headset_action(QString action)
{
	if (!current || !current->messaging)
		return;
	if (action == "SwitchMonitor") // Windows BindingSettings: DynamicSettings.SwitchMonitor() on the PC side
		return on_monitor_message(uint8_t(message_type::switch_monitor), {});
	if (action == "ToggleVRMode")
	{
		if (!vr_available())
			return info("[bindings] ToggleVRMode: no VR app is running");
		int source = current->streamer.value("StreamingSource").toInt();
		send_streamer_settings({{"StreamingSource", source == 1 ? 0 : 1}}, false);
		return;
	}
	// Upstream.Net.MessageType values the headset reacts to
	static const QMap<QString, uint8_t> types{
	        {"EnableVRPassthrough", 72},   {"DisableVRPassthrough", 73},
	        {"ToggleVRPassthrough", 74},  {"ToggleHandPassthrough", 75}, {"ToggleDeskPassthrough", 76},
	        {"TogglePerformanceOverlay", 77}, {"ToggleFoveatedStreaming", 79}};
	auto it = types.find(action);
	if (it == types.end())
		return;
	bytes chunk = current->writer->encode(encode_net_message(message_type(it.value()), nullptr, 0));
	current->messaging->write(reinterpret_cast<const char *>(chunk.data()), qint64(chunk.size()));
	info("[bindings] -> " + action);
}

void native_engine::on_discovery(QUdpSocket * socket)
{
	while (socket->hasPendingDatagrams())
	{
		QNetworkDatagram dg = socket->receiveDatagram();
		QByteArray d = dg.data();
		QString host = host_of(dg.senderAddress());
		auto * p = reinterpret_cast<const uint8_t *>(d.constData());

		if (d.size() == 17) // headset selected this computer: ConnectionID + platform
		{
			if (std::equal(computer.connection_id.begin(), computer.connection_id.end(), p))
			{
				emit_event("selected", {{"host", host}, {"platform", int(p[16])}});
				info(QString("headset %1 selected this computer").arg(host));
			}
			continue;
		}
		if (d.size() <= 243) // RSA key XML alone is 243 bytes
			continue;
		auto req = parse_discovery_request(p, d.size());
		if (!req)
			continue;
		QString account = QString::fromStdString(req->account_id);
		if (!accounts.contains(account))
		{
			info(QString("ignoring discovery from %1: account '%2' not allowed").arg(host, account));
			emit_event("pairing_request", {{"account", account}, {"platform", req->platform}, {"host", host}});
			continue;
		}
		computer.adapters = local_adapters(); // USB link-local appears once the headset brings the link up
		auto reply = build_discovery_reply(*req, computer);
		if (!reply)
			continue;
		socket->writeDatagram(reinterpret_cast<const char *>(reply->data()), qint64(reply->size()), dg.senderAddress(), dg.senderPort());
		info(QString("discovery reply -> %1 (account '%2', %3 bytes)").arg(host, account).arg(reply->size()));
	}
}

void native_engine::send_streamer_settings(const QJsonObject & values, bool reload)
{
	if (!current || !current->messaging)
		return;
	if (reload)
		current->streamer = {};
	for (auto it = values.begin(); it != values.end(); ++it)
		current->streamer[it.key()] = it.value();
	std::string json = QJsonDocument(values).toJson(QJsonDocument::Compact).toStdString();
	auto type = reload ? message_type::streamer_settings_reload : message_type::streamer_settings_property_changed;
	bytes chunk = current->writer->encode(encode_net_message(type, json));
	current->messaging->write(reinterpret_cast<const char *>(chunk.data()), qint64(chunk.size()));
	info(QString("[messaging] -> %1 %2").arg(message_name(uint8_t(type)), QString::fromStdString(json)));
}

void native_engine::on_messaging_connection()
{
	while (QTcpSocket * s = messaging_server->nextPendingConnection())
	{
		s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
		QString host = host_of(s->peerAddress());
		current = std::make_unique<session>();
		current->messaging = s;
		current->encrypted = computer.encrypt_local;
		current->pulse = pulse_audio();
		current->reader = std::make_unique<aes_stream_reader>(computer.key, computer.iv);
		current->writer = std::make_unique<aes_stream_writer>(computer.key, computer.iv);
		info(QString("[messaging] connection from %1").arg(host));
		emit_event("connected", {{"host", host}, {"usb", s->peerAddress().isLinkLocal()}});

		// headset is a client of SharedStreamerSettings: it waits for our Reload
		// like Windows: HasVR (and VR mode) only while a VR app has a session
		QJsonObject settings{{"HasVR", vr_available()},
		                     {"AdapterName", options.adapter_name.isEmpty() ? "Linux" : options.adapter_name},
		                     {"AdapterIsAMD", options.adapter_is_amd},
		                     {"AutoAdjustBitrate", options.auto_adjust_bitrate},
		                     {"InputLanguage", input_language()}};
		if (vr_available())
			settings["ActiveRuntime"] = 2; // OpenDesk XR
		if (vr && vr->has_client())
			settings["StreamingSource"] = 1; // a VR app is already running
		send_streamer_settings(settings, true);

		session * sess = current.get();
		connect(s, &QTcpSocket::readyRead, this, [this, s, sess] {
			if (current.get() != sess)
				return;
			QByteArray d = s->readAll();
			sess->reader->feed(reinterpret_cast<const uint8_t *>(d.constData()), d.size());
			if (sess->reader->error())
			{
				info("[messaging] stream decrypt error, dropping connection");
				s->abort();
				return;
			}
			while (auto m = pop_net_message(sess->reader->plaintext()))
			{
				const char * name = message_name(m->type);
				QString qname = name ? name : QString("type%1").arg(m->type);
				QByteArray payload(reinterpret_cast<const char *>(m->payload.data()), qsizetype(m->payload.size()));
				QJsonObject * target = nullptr;
				if (qname.startsWith("UserSettings"))
					target = &sess->user;
				else if (qname.startsWith("MobileSettings"))
					target = &sess->mobile;
				else if (qname == "StreamerSettingsPropertyChanged")
					target = &sess->streamer;
				if (target)
				{
					QJsonObject values = QJsonDocument::fromJson(payload).object();
					if (qname.endsWith("Reload"))
						*target = {};
					for (auto it = values.begin(); it != values.end(); ++it)
						(*target)[it.key()] = it.value();
					emit_event("settings", {{"kind", qname}, {"values", values}});
					static const QStringList video_keys{"DesktopFramerate", "DesktopBitrateLimit", "MeasuredBandwidth", "HmdType",
					                                    "VRFramerate", "VRGraphicsQuality", "VRBitrateLimit",
					                                    "IncreaseVideoNominalRange"};
					for (const QString & k: video_keys)
						if (values.contains(k))
						{
							update_video();
							break;
						}
				}
				if ((qname == "ForceKeyFrame" || qname == "RestartStream") && sess->video)
					sess->video->request_keyframe();
				if (qname.startsWith("UserSettings") && sess->user.contains("MicVolume") && sess->mic)
					sess->mic->set_volume(float(sess->user.value("MicVolume").toDouble(0.85)));
				if (qname.startsWith("UserSettings") && sess->mic && !sess->user.value("MicPassthrough").toBool(true))
				{
					info("[audio] headset microphone passthrough turned off: 'OpenDesk Microphone' removed");
					sess->undo_mic();
				}
				if (m->type == uint8_t(message_type::update_required))
				{
					// The headset offers to update the streamer. There is nothing to update, so decline and keep the
					// session; the protocol is unchanged across patch releases. Compatibility mode advertises a low
					// version, so this arrives on every connect with a newer headset app - log it once, quietly.
					bytes chunk = sess->writer->encode(encode_net_message(message_type::update_unavailable, nullptr, 0));
					s->write(reinterpret_cast<const char *>(chunk.data()), qint64(chunk.size()));
					if (!std::exchange(sess->newer_headset_reported, true))
						info(QString("[update] headset asked for a streamer update; staying connected (reported version %1)")
						             .arg(QString::fromStdString(version_string(computer.version))));
				}
				if (qname == "DisposeGamepadEmulation" && sess->gamepad)
				{
					info("[input] virtual gamepad removed (headset stopped emulating)");
					sess->gamepad.reset();
				}
				if (m->type == uint8_t(message_type::skeleton_data_changed) && vr)
					vr->set_skeleton(m->payload.data(), m->payload.size());
				if (m->type == uint8_t(message_type::recenter)) {
					++options.render.recenter_serial;
					update_vr_server();
				}
				if (m->type == uint8_t(message_type::screenshot_to_desktop))
					save_screenshot(m->payload);
#ifndef _WIN32
				if (m->type == uint8_t(message_type::change_language)) // layoutChanged then updates InputLanguage
					QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(
					        "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "switchToNextLayout"));
#else
				if (m->type == uint8_t(message_type::change_language))
					ActivateKeyboardLayout(reinterpret_cast<HKL>(static_cast<intptr_t>(HKL_NEXT)), 0);
#endif
				if (m->type == uint8_t(message_type::switch_monitor) || m->type == uint8_t(message_type::add_monitor) ||
				    m->type == uint8_t(message_type::remove_monitor) || m->type == uint8_t(message_type::set_monitor_portrait))
					on_monitor_message(m->type, m->payload);
				if (qname.startsWith("UserSettings") && QJsonDocument::fromJson(payload).object().contains("ExtraDisplay"))
					sync_extra_display();
				if (m->type >= uint8_t(message_type::request_videos) && m->type <= uint8_t(message_type::set_subtitle_offset_amount))
					on_video_message(m->type, m->payload);
				if (m->type >= uint8_t(message_type::request_games) && m->type <= uint8_t(message_type::exit_game))
					on_games_message(m->type, m->payload);
				if (qname.startsWith("MobileSettings"))
				{
					std::lock_guard lock(sess->vr_tracking->m);
					sess->vr_tracking->ipd = float(sess->mobile.value("IPD").toDouble(0.063));
					for (int e = 0; e < 2; ++e)
					{
						QJsonObject f = sess->mobile.value(e == 0 ? "FovLeft" : "FovRight").toObject();
						sess->vr_tracking->fov[e] = {float(f.value("UpTan").toDouble(1)), float(f.value("DownTan").toDouble(1)),
						                             float(f.value("LeftTan").toDouble(1)), float(f.value("RightTan").toDouble(1))};
					}
				}
				if (qname.startsWith("MobileSettings") || qname.startsWith("UserSettings") || qname == "StreamerSettingsPropertyChanged")
					update_vr_server();
				if (qname == "StreamerSettingsPropertyChanged" && QJsonDocument::fromJson(payload).object().contains("StreamingSource"))
					update_video(true); // headset toggled VR mode
				if (qname.startsWith("MobileSettings") && QJsonDocument::fromJson(payload).object().contains("FoveatedStreaming"))
				{
					info(QString("[vr] foveated streaming %1").arg(sess->mobile.value("FoveatedStreaming").toBool() ? "on" : "off"));
					update_video(true);
				}
				if (qname.startsWith("MobileSettings") && sess->player)
				{
					QJsonObject v = QJsonDocument::fromJson(payload).object();
					if (v.contains("IsVideoPaused"))
						sess->player->set_paused(v["IsVideoPaused"].toBool());
					if (v.contains("VideoPlaybackSpeed"))
						sess->player->set_speed(float(v["VideoPlaybackSpeed"].toDouble(1)));
				}
				// preview JSON settings; binary payloads (screenshots, clipboard images) only by size
				QString preview = payload.startsWith('{') ? QString::fromUtf8(payload.left(96)) : QString();
				info(QString("[messaging] <- %1 len=%2 %3").arg(qname).arg(payload.size()).arg(preview));
			}
		});
		connect(s, &QTcpSocket::disconnected, this, [this, s, host] {
			info(QString("[messaging] %1 disconnected").arg(host));
			emit_event("disconnected", {{"host", host}});
			if (current && current->messaging == s)
			{
				stop_video_source(current.get());
				current.reset();
				update_vr_server();
				if (options.lock_on_disconnect)
				{
					info("locking the session (Lock computer on disconnect)");
					QProcess::startDetached("loginctl", {"lock-session"});
				}
			}
			s->deleteLater();
		});
	}
}

void native_engine::on_data_connection()
{
	while (QTcpSocket * s = data_server->nextPendingConnection())
	{
		s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
		info(QString("[data] connection from %1").arg(host_of(s->peerAddress())));
		if (!current)
		{
			s->abort();
			continue;
		}
		current->data = s;
		connect(s, &QTcpSocket::bytesWritten, this, [this, sess = current.get()] {
			if (current.get() == sess)
				pump_download(sess);
		});

		// NetDataClient.TryMeasureBitrate: i32 BE N, then N bytes; the headset times the second half. Like Windows,
		// only with "Auto-adjust bitrate" (5 MB); N = 0 leaves MeasuredBandwidth at 1 Gbit/s (no cap) and the
		// headset's performance overlay shows "Automatic bitrate: No"
		const int probe_bytes = options.auto_adjust_bitrate ? options.probe_bytes : 0;
		bytes probe;
		append_be32(probe, probe_bytes);
		QByteArray random(1 << 20, Qt::Uninitialized);
		random_bytes(reinterpret_cast<uint8_t *>(random.data()), random.size());
		if (current->encrypted)
		{
			current->data_writer = std::make_unique<aes_stream_writer>(computer.key, computer.iv);
			aes_stream_writer & w = *current->data_writer;
			bytes c = w.encode(probe);
			s->write(reinterpret_cast<const char *>(c.data()), qint64(c.size()));
			for (int left = probe_bytes; left > 0; left -= random.size())
			{
				c = w.encode(reinterpret_cast<const uint8_t *>(random.constData()), size_t(std::min<qint64>(left, random.size())));
				s->write(reinterpret_cast<const char *>(c.data()), qint64(c.size()));
			}
			current->data_reader = std::make_unique<aes_stream_reader>(computer.key, computer.iv);
		}
		else
		{
			s->write(reinterpret_cast<const char *>(probe.data()), 4);
			for (int left = probe_bytes; left > 0; left -= random.size())
				s->write(random.constData(), std::min<qint64>(left, random.size()));
		}
		info(QString("[data] -> bandwidth probe %1 bytes").arg(probe_bytes));
		session * probe_sess = current.get();
		QTimer::singleShot(1000, this, [this, probe_sess] { // let the headset flip IsConnected first
			if (current.get() != probe_sess)
				return;
			probe_sess->video_ready = true;
			update_video();
		});

		// laser pointers send desktop coordinates; until the video knows the monitors, assume the GUI's primary screen
		try
		{
			current->injector = std::make_unique<input_injector>();
			current->injector->set_touch_mode(options.touch);
			int w = options.desktop_width, h = options.desktop_height;
			if (w > 0 && h > 0)
				current->injector->set_layout({w, h, {{{0, 0, w, h}, {}}}});
			info(QString("uinput devices ready (pointers as %1)").arg(options.touch ? "touch" : "mouse"));
		}
		catch (const std::exception & e)
		{
			info(QString("input injection disabled: %1").arg(e.what()));
		}

		session * sess = current.get();
		connect(s, &QTcpSocket::readyRead, this, [this, s, sess] {
			if (current.get() != sess)
				return s->readAll(), void();
			QByteArray d = s->readAll();
			if (sess->data_reader)
			{
				sess->data_reader->feed(reinterpret_cast<const uint8_t *>(d.constData()), d.size());
				bytes & plain = sess->data_reader->plaintext();
				append(sess->data_buffer, plain.data(), plain.size());
				plain.clear();
			}
			else
				append(sess->data_buffer, d.constData(), d.size());
			// FrameData packet: i32 LE lz4 size, u64 xxh3, LZ4 block
			while (sess->data_buffer.size() >= 12)
			{
				int32_t size = read_le<int32_t>(sess->data_buffer.data());
				if (size < 0 || sess->data_buffer.size() < size_t(size) + 12)
					break;
				bytes frame(frame_data_size);
				int n = LZ4_decompress_safe(reinterpret_cast<const char *>(sess->data_buffer.data() + 12),
				                            reinterpret_cast<char *>(frame.data()), size, int(frame.size()));
				sess->data_buffer.erase(sess->data_buffer.begin(), sess->data_buffer.begin() + 12 + size);
				if (n < 0)
					continue;
				frame.resize(n);
				if (sess->frame_data_packets++ == 0)
					info(QString("[data] <- first FrameData %1 bytes").arg(n));
				if (sess->injector)
					sess->injector->apply(frame);
				update_gamepad(sess, frame);
				{
					std::lock_guard lock(sess->vr_tracking->m);
					sess->vr_tracking->frame = frame;
					int32_t index = read_le<int32_t>(frame.data());
					sess->vr_tracking->arrivals[size_t(uint32_t(index)) % sess->vr_tracking->arrivals.size()] = {
					        index, std::chrono::steady_clock::now().time_since_epoch().count()};
				}
				if (vr)
					vr->update_tracking(frame.data(), frame.size());
				if (face)
					face->on_frame_data(frame.data(), frame.size());
				sess->latest_frame_data = std::move(frame);
			}
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
}

void native_engine::on_audio_connection()
{
	while (QTcpSocket * s = audio_server->nextPendingConnection())
	{
		s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
		info(QString("[audio] connection from %1").arg(host_of(s->peerAddress())));
		if (!current)
		{
			s->abort();
			continue;
		}
		current->audio_socket = s;
		current->audio_out = std::make_unique<stream_channel>(int(s->socketDescriptor()), channel_encryption(current.get()));
		if (current->encrypted)
			current->audio_reader = std::make_unique<aes_stream_reader>(computer.key, computer.iv);
		setup_audio_output(current.get());

		session * sess = current.get();
		connect(s, &QTcpSocket::readyRead, this, [this, s, sess] {
			if (current.get() != sess)
				return s->readAll(), void();
			QByteArray d = s->readAll();
			if (sess->audio_reader)
			{
				sess->audio_reader->feed(reinterpret_cast<const uint8_t *>(d.constData()), d.size());
				bytes & plain = sess->audio_reader->plaintext();
				append(sess->mic_buffer, plain.data(), plain.size());
				plain.clear();
			}
			else
				append(sess->mic_buffer, d.constData(), d.size());
			while (sess->mic_buffer.size() >= 4) // headset microphone: i32 LE size + s16 mono PCM
			{
				int32_t size = read_le<int32_t>(sess->mic_buffer.data());
				if (size < 0 || sess->mic_buffer.size() < size_t(size) + 4)
					break;
				on_mic_packet(sess, sess->mic_buffer.data() + 4, size_t(size));
				sess->mic_buffer.erase(sess->mic_buffer.begin(), sess->mic_buffer.begin() + 4 + size);
			}
		});
		connect(s, &QTcpSocket::disconnected, this, [this, s] {
			if (current && current->audio_socket == s)
			{
				current->undo_audio_output(); // joins the sender before the fd goes away
				current->undo_mic();
				current->player.reset();
				current->audio_out.reset();
			}
			s->deleteLater();
		});
	}
}

void native_engine::on_video_connection()
{
	while (QTcpSocket * s = video_server->nextPendingConnection())
	{
		s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
		info(QString("[video] connection from %1").arg(host_of(s->peerAddress())));
		if (!current)
		{
			s->abort();
			continue;
		}
		current->video_socket = s;
		// harness: the send buffer TCP grows to over Wi-Fi (on loopback it stays small, hiding a standing queue)
		if (const char * b = getenv("OPENDESK_TEST_VIDEO_SNDBUF"))
		{
			int size = atoi(b);
			setsockopt(int(s->socketDescriptor()), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&size), sizeof(size));
		}
		current->video_out = std::make_unique<stream_channel>(int(s->socketDescriptor()), channel_encryption(current.get()));
		// the headset sends eye-gaze positions here for foveated streaming: [i32 16][left x, y][right x, y] (floats,
		// [-0.5, 0.5] of each eye's FOV, +y down) every 11 ms while the VR app streams foveated
		if (current->encrypted)
			current->video_reader = std::make_unique<aes_stream_reader>(computer.key, computer.iv);
		session * sess = current.get();
		connect(s, &QTcpSocket::readyRead, this, [this, s, sess] {
			QByteArray d = s->readAll();
			if (current.get() != sess)
				return;
			if (sess->video_reader)
			{
				sess->video_reader->feed(reinterpret_cast<const uint8_t *>(d.constData()), d.size());
				bytes & plain = sess->video_reader->plaintext();
				append(sess->video_in, plain.data(), plain.size());
				plain.clear();
			}
			else
				append(sess->video_in, d.constData(), d.size());
			while (sess->video_in.size() >= 4)
			{
				int32_t size = read_le<int32_t>(sess->video_in.data());
				if (size < 0 || size > 4096)
				{
					sess->video_in.clear();
					break;
				}
				if (sess->video_in.size() < size_t(size) + 4)
					break;
				if (size == 16 && vr)
				{
					float g[2][2];
					std::memcpy(g, sess->video_in.data() + 4, 16);
					// per eye as sent; the runtime falls back to the mean direction when the eyes disagree implausibly
					vr->set_gaze(true, g);
					if (!sess->gaze_seen)
					{
						sess->gaze_seen = true;
						info(QString("[vr] eye tracking: gaze %1,%2 / %3,%4 (per eye)").arg(g[0][0]).arg(g[0][1]).arg(g[1][0]).arg(g[1][1]));
					}
				}
				sess->video_in.erase(sess->video_in.begin(), sess->video_in.begin() + 4 + size);
			}
		});
		connect(s, &QTcpSocket::disconnected, this, [this, s] {
			if (current && current->video_socket == s)
			{
				stop_video_source(current.get()); // join the senders before the fd goes away
				current->player.reset();
				current->video_out.reset();
			}
			s->deleteLater();
		});
		update_video();
	}
}

void native_engine::update_video(bool force)
{
	if (!current || !current->video_socket || !current->video_ready)
		return;
	if (current->capture_failed) // don't re-prompt in a loop; the next connection tries again
		return;
	if (current->player) // a video file owns the video channel
		return;
	if (current->streamer.value("StreamingSource").toInt() == 1)
		return start_vr(force);

	// SharedMobileSettings: DesktopFramerate [DefaultValue(60)];
	// UpdateDesktopBitrate: min(MeasuredBandwidth, lerp(4 Mbps, 120 Mbps, DesktopBitrateLimit))
	desktop_video_params p;
	p.fps = current->mobile.value("DesktopFramerate").toInt(60);
	double limit = std::round(current->user.value("DesktopBitrateLimit").toDouble(0) * 1000) / 1000;
	int hmd = current->mobile.value("HmdType").toInt(-1);
	double max_bitrate = hmd == 259 ? 40e6 : (hmd == 1700 || hmd == 1710) ? 200e6 : 120e6; // GetMaxDesktopBitrate
	int64_t wanted = int64_t(4e6 + (max_bitrate - 4e6) * limit);
	int64_t measured = current->mobile.value("MeasuredBandwidth").toVariant().toLongLong();
	int64_t bitrate = measured > 0 ? std::min(wanted, measured) : wanted; // like the headset's DesktopBitrate
	p.kbps = int(std::max<int64_t>(bitrate / 1000, 1000));
	// Windows StreamerManager: Automatic prefers HEVC 10-bit (falls back if the GPU can't), H.264+ is a VR
	// multi-stream mode -> plain H.264 for the desktop
	switch (options.codec_id)
	{
		case 1:
		case 5: p.codec = video_codec::h264; break;
		case 2: p.codec = video_codec::hevc; break;
		case 10: p.codec = video_codec::av1; break;
		case 11: p.codec = video_codec::av1_10bit; break;
		default: p.codec = video_codec::hevc_10bit; break; // 0 Automatic, 6 HEVC 10-bit
	}
	p.full_range = current->user.value("IncreaseVideoNominalRange").toBool();
	p.vbaq = options.aq;
	p.pre_encode = options.two_pass;

	if (current->video && current->video->running() && p == current->video_params && !force)
		return;
	stop_video_source(current.get()); // stop capturing before streams are reopened
	current->video_params = p;
	session * sess = current.get();

	// which monitors exist (KWin: right away; portal: the share dialog the first time)
	if (!sess->displays)
	{
		sess->displays = std::make_unique<display_manager>();
		if (!sess->displays->is_kwin())
			info(sess->displays->is_x11() ? QString("[capture] X11 session: monitors from XRandR, grabbed directly (MIT-SHM)")
			                               : "[capture] KWin screen cast unavailable (" + sess->displays->kwin_unavailable_reason() +
			                                         ") - using the desktop portal (no virtual monitors)");
		connect(sess->displays.get(), &display_manager::restore_token, this, [this](QString t) { save_portal_token(t); });
		connect(sess->displays.get(), &display_manager::log, this, &native_engine::info);
#ifdef _WIN32
		// the Virtual Display Driver's monitors appear a few seconds after the request: the extra display becomes the
		// main screen then (displays_changed restarts the stream)
		connect(sess->displays.get(), &display_manager::virtual_ready, this, [this, sess](QString name) {
			if (current.get() != sess || !sess->extra_display || sess->has_switched_monitor)
				return;
			auto all = sess->displays->displays();
			for (size_t i = 0; i < all.size(); ++i)
				if (all[i].name == name.toStdString())
					sess->monitor_start = int(i);
		});
#endif
		connect(sess->displays.get(), &display_manager::displays_changed, this, [this, sess] {
			if (current.get() != sess || !sess->displays)
				return;
			std::string sig;
			for (const auto & d: sess->displays->displays())
				sig += std::format("{}:{},{},{}x{};", d.name, d.x, d.y, d.width, d.height);
			if (sig != sess->layout_signature)
			{
				info("[capture] monitors changed - updating the stream");
				schedule_video_restart(1000);
			}
		});
	}
	if (!sess->displays->ready())
	{
		if (sess->displays_starting)
			return;
		sess->displays_starting = true;
		if (!sess->displays->is_x11())
			info(portal_restore_token.isEmpty() ? "[capture] asking the desktop for screen access (first time: approve the dialog)"
		                                    : "[capture] restoring the saved screen-share permission");
		sess->displays->start(portal_restore_token, [this, sess](QString error) {
			QTimer::singleShot(0, this, [this, sess, error] { // not from inside the portal's callback
				if (current.get() != sess)
					return;
				sess->displays_starting = false;
				if (!error.isEmpty())
				{
					info("[capture] screen capture failed: " + error);
					emit_event("capture_error", {{"message", error}});
					sess->capture_failed = true; // don't re-prompt in a loop; the next connection tries again
					return;
				}
				update_video(true);
			});
		});
		return;
	}
	sync_extra_display();

	std::vector<display_info> all = sess->displays->displays();
	if (all.empty())
		return info("[capture] no monitor to stream");
	sess->layout_signature.clear();
	for (const auto & d: all)
		sess->layout_signature += std::format("{}:{},{},{}x{};", d.name, d.x, d.y, d.width, d.height);
	int n = int(all.size());
	int max_screens = std::min(max_monitors(sess->mobile.value("HmdType").toInt(-1)), 4);
	if (monitor_count <= 0)
		monitor_count = std::min(n, max_screens);
	int count = std::clamp(monitor_count, 1, std::min(n, max_screens));
	if (sess->monitor_start >= n)
		sess->monitor_start = 0;
	sess->streamed.clear();
	for (int i = 0; i < count; ++i)
		sess->streamed.push_back(all[size_t((sess->monitor_start + i) % n)]);
	update_monitor_flags(n);

	// Bounds: desktop (logical) coordinates from the top-left of all monitors, scaled up to the densest streamed
	// screen so pointers keep pixel precision on HiDPI; int16 on the wire
	int min_x = INT32_MAX, min_y = INT32_MAX, max_x = INT32_MIN, max_y = INT32_MIN;
	for (const auto & d: all)
	{
		min_x = std::min(min_x, d.x);
		min_y = std::min(min_y, d.y);
		max_x = std::max(max_x, d.x + d.width);
		max_y = std::max(max_y, d.y + d.height);
	}
	double k = 1;
	for (const auto & d: sess->streamed)
		if (d.width > 0 && d.pixel_width > 0)
			k = std::max(k, double(d.pixel_width) / d.width);
	k = std::min(k, 32000.0 / std::max(max_x - min_x, max_y - min_y));
	auto scaled = [k](int v) { return int(std::lround(v * k)); };

	std::vector<desktop_stream_setup> setups;
	input_layout layout{scaled(max_x - min_x), scaled(max_y - min_y), {}};
	std::set<std::string> in_use;
	for (const auto & d: sess->streamed)
	{
		QString error;
		auto setup = sess->displays->open(d, &error);
		if (!setup)
		{
			info(QString("[capture] cannot capture %1: %2").arg(QString::fromStdString(d.name), error));
#ifndef _WIN32
			for (auto & s: setups)
				if (s.pipewire_fd >= 0)
					::close(s.pipewire_fd);
#endif
			return; // the watchdog retries
		}
		setup->bounds = {scaled(d.x - min_x), scaled(d.y - min_y), scaled(d.width), scaled(d.height)};
		layout.screens.push_back({setup->bounds, sess->displays->is_kwin() ? d.name : std::string()});
		in_use.insert(d.name);
		setups.push_back(std::move(*setup));
	}
	sess->displays->release_unused(in_use);

	auto logger = [this](std::string line) { QMetaObject::invokeMethod(this, [this, line] { info(QString::fromStdString(line)); }); };
	desktop_video_sender::callbacks cb{logger, [this, sess](video_codec c) {
		                                   QMetaObject::invokeMethod(this, [this, sess, c] {
			                                   if (current.get() != sess)
				                                   return;
			                                   send_streamer_settings({{"ActiveCodec", int(c)}}, false);
			                                   emit_event("video", {{"mode", "desktop"}, {"fps", sess->video_params.fps},
			                                                        {"kbps", sess->video_params.kbps}, {"codec", codec_name(c)},
			                                                        {"screens", int(sess->streamed.size())}});
		                                   });
	                                   }};
	try
	{
		sess->video = std::make_unique<desktop_video_sender>(*sess->video_out, p, std::move(setups), cb);
	}
	catch (const std::exception & e)
	{
		info(QString("[capture] cannot start the capture: %1").arg(e.what()));
		return;
	}
	if (sess->injector && sess->injector->set_layout(layout))
	{
		info(QString("[input] pointer layout %1x%2, %3 screen(s)").arg(layout.width).arg(layout.height).arg(layout.screens.size()));
		if (sess->displays->is_kwin())
			map_touch_outputs();
	}
	emit_event("video", {{"mode", "desktop"}, {"fps", p.fps}, {"kbps", p.kbps}, {"codec", codec_name(p.codec)}, {"screens", count}});
}

bool native_engine::vr_test_pattern()
{
	static const bool on = qEnvironmentVariableIsSet("OPENDESK_VR_TEST_PATTERN"); // dev aid: grid without an app
	return on;
}

bool native_engine::vr_available() const
{
	return (vr && vr->rendering()) || vr_linger || vr_test_pattern();
}

// a game launched from the headset that hasn't rendered yet (Proton + shader caches can take a while)
bool native_engine::game_launch_pending() const
{
	return QDateTime::currentMSecsSinceEpoch() < vr_wait_until_ms;
}

void native_engine::sync_vr_state()
{
	if (!current || !current->messaging)
		return;
	bool available = vr_available();
	bool in_vr = current->streamer.value("StreamingSource").toInt() == 1;
	QJsonObject values{{"HasVR", available}};
	if (available)
	{
		values["ActiveRuntime"] = 2; // OpenDesk XR
		if (!in_vr && vr && vr->rendering())
			values["StreamingSource"] = 1; // an app renders: switch the headset to VR, like Windows
	}
	else if (in_vr && !game_launch_pending())
		values["StreamingSource"] = 0;
	for (auto it = values.begin(); it != values.end();) // only what changes
		it = current->streamer.value(it.key()) == it.value() ? values.erase(it) : std::next(it);
	if (!values.isEmpty())
		send_streamer_settings(values, false);
	update_vr_server();
	update_video(true);
}

void native_engine::stop_video_source(session * sess)
{
	if (vr)
		vr->set_sink(nullptr); // the server must drop its reference before the sender goes away
	if (sess)
		sess->video.reset();
}

#ifdef _WIN32
// Windows: xrizer comes with the installer (<install>\xrizer) or OPENDESK_XRIZER; there is no download step
void native_engine::fetch_xrizer()
{
	if (xrizer_fetch_started)
		return;
	xrizer_fetch_started = true;
	info("[openvr] xrizer is not installed: OpenVR games need SteamVR (reinstall OpenDesk or set OPENDESK_XRIZER)");
}

// "Boost game priority": the VR app's process above normal and its render thread at highest priority. Both work on
// the user's own processes without administrator rights (elevated games refuse; that is reported).
void native_engine::apply_game_priority()
{
	if (options.boost_priority && render_tid > 0 && boosted_tid != render_tid)
	{
		HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(render_pid));
		HANDLE thread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, DWORD(render_tid));
		bool ok = thread && SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
		if (process)
		{
			if (GetPriorityClass(process) == NORMAL_PRIORITY_CLASS)
				SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS);
			CloseHandle(process);
		}
		if (thread)
			CloseHandle(thread);
		if (ok)
		{
			boosted_tid = render_tid;
			info(QString("[priority] VR app (pid %1): render thread %2 at highest priority").arg(render_pid).arg(render_tid));
		}
		else
			info(QString("[priority] cannot raise the VR app's priority (error %1)").arg(GetLastError()));
	}
	else if (!options.boost_priority && boosted_tid > 0)
	{
		if (HANDLE thread = OpenThread(THREAD_SET_INFORMATION, FALSE, DWORD(boosted_tid)))
		{
			SetThreadPriority(thread, THREAD_PRIORITY_NORMAL);
			CloseHandle(thread);
		}
		if (HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(render_pid)))
		{
			if (GetPriorityClass(process) == ABOVE_NORMAL_PRIORITY_CLASS)
				SetPriorityClass(process, NORMAL_PRIORITY_CLASS);
			CloseHandle(process);
		}
		info(QString("[priority] VR app render thread %1 back to normal priority").arg(boosted_tid));
		boosted_tid = 0;
	}
}
#else
// our xrizer (never a system install) is missing: tools/get_xrizer.sh downloads xrizer's CI nightly, once per run
void native_engine::fetch_xrizer()
{
	if (xrizer_fetch_started)
		return;
	xrizer_fetch_started = true;
	auto * p = new QProcess(this);
	p->setProcessChannelMode(QProcess::MergedChannels);
	connect(p, &QProcess::finished, this, [this, p](int code) {
		QString out = QString::fromUtf8(p->readAll()).trimmed();
		p->deleteLater();
		if (code == 0)
		{
			info("[openvr] " + out.section('\n', -1));
			update_vr_server(); // now register it
		}
		else
			info("[openvr] could not get xrizer (OpenVR games need SteamVR until the System check fixes it): " +
			     out.section('\n', -3));
	});
	info("[openvr] getting xrizer for OpenVR games (xrizer's nightly build) ...");
	p->start("sh", {od::resource_dir("tools", OPENDESK_TOOLS_DIR) + "/get_xrizer.sh", "nightly"});
}

// Windows raises the game's GPU scheduling priority through upstream's admin service; on Linux that takes root
// (amdgpu/NVIDIA context priorities above normal need CAP_SYS_NICE), so the render thread gets a higher CPU priority
// instead, through rtkit (the desktop's priority broker: up to nice -15 for the user's own threads, no root)
void native_engine::apply_game_priority()
{
	if (options.boost_priority && render_tid > 0 && boosted_tid != render_tid)
	{
		QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
		                                                   "org.freedesktop.RealtimeKit1", "MakeThreadHighPriorityWithPID");
		call << qulonglong(render_pid) << qulonglong(render_tid) << int(-10);
		errno = 0;
		const int before = getpriority(PRIO_PROCESS, id_t(render_tid));
		QDBusMessage reply = QDBusConnection::systemBus().call(call, QDBus::Block, 2000);
		if (reply.type() == QDBusMessage::ReplyMessage)
		{
			boosted_tid = render_tid;
			boosted_from_nice = errno ? 0 : before;
			info(QString("[priority] VR app (pid %1): render thread %2 at nice -10").arg(render_pid).arg(render_tid));
		}
		else
			info(QString("[priority] cannot raise the VR app's priority (rtkit: %1)").arg(reply.errorMessage()));
	}
	else if (!options.boost_priority && boosted_tid > 0)
	{
		setpriority(PRIO_PROCESS, id_t(boosted_tid), boosted_from_nice); // back to where it was: lowering needs no rights
		info(QString("[priority] VR app render thread %1 back to normal priority").arg(boosted_tid));
		boosted_tid = 0;
	}
}

#endif

void native_engine::update_vr_server()
{
	// system OpenXR runtime: ours while the streamer runs, the user's own again afterwards
	if (!runtime_registered)
	{
		std::string error = openxr_runtime::activate(od::resource_dir("runtime", OPENDESK_RUNTIME_DIR).toStdString());
		if (error.empty())
		{
			runtime_registered = true;
#ifdef _WIN32
			info(QString("[openxr] OpenDesk XR is the active OpenXR runtime (%1)")
			             .arg(QString::fromStdString(openxr_runtime::installed_manifest())));
#else
			info(QString("[openxr] OpenDesk XR is the active OpenXR runtime (%1); restored when the streamer exits")
			             .arg(QString::fromStdString(openxr_runtime::installed_manifest())));
#endif
		}
		else if (error.starts_with("pending"))
		{
			if (!runtime_pending_logged)
				info(QString("[openxr] %1").arg(QString::fromStdString(error.substr(9))));
			runtime_pending_logged = true;
		}
		else
			info(QString("[openxr] cannot register OpenDesk XR: %1").arg(QString::fromStdString(error)));
	}
	// OpenVR games: xrizer (on top of OpenDesk XR) with our OpenXR runtime
	if (runtime_registered && !openvr_registered)
	{
		std::string xrizer = openvr_runtime::find_xrizer();
		std::string error = xrizer.empty() ? std::string("fetching") : openvr_runtime::activate(xrizer);
		if (xrizer.empty())
			fetch_xrizer(); // registers when it's there
		else if (error.empty())
		{
			openvr_registered = true;
			info(QString("[openvr] xrizer (%1) is the OpenVR runtime: OpenVR games run on OpenDesk XR; restored when the "
			             "streamer exits").arg(QString::fromStdString(xrizer)));
		}
		else if (!xrizer.empty() && !openvr_warned)
		{
			openvr_warned = true;
			info(QString("[openvr] OpenVR games need SteamVR: %1").arg(QString::fromStdString(error)));
		}
	}
	if (!vr)
	{
		try
		{
			vr_server::callbacks cb;
			cb.log = [this](std::string l) { QMetaObject::invokeMethod(this, [this, l] { info(QString::fromStdString(l)); }); };
			cb.client = [this](bool connected, std::string app) {
				QMetaObject::invokeMethod(this, [this, connected, app] {
					vr_app = connected ? QString::fromStdString(app) : QString();
					emit_event("vr_app", {{"connected", connected}, {"app", vr_app}});
					if (connected)
						return; // VR mode once it renders (first_frame)
					if (!std::exchange(vr_app_rendered, false))
						return update_vr_server(); // a probing client: nothing changed for the headset
					// app gone: back to the desktop unless another one renders within a moment (launchers and
					// Proton's helpers come and go before the game itself)
					vr_linger = true;
					QTimer::singleShot(3000, this, [this] {
						vr_linger = false;
						sync_vr_state();
					});
					update_vr_server();
				});
			};
			cb.first_frame = [this] {
				QMetaObject::invokeMethod(this, [this] {
					vr_app_rendered = true;
					vr_linger = false;
					vr_wait_until_ms = 0;
					sync_vr_state(); // VR app renders: headset into VR, like Windows
				});
			};
			cb.render_thread = [this](int pid, int tid) {
				QMetaObject::invokeMethod(this, [this, pid, tid] {
					render_pid = pid;
					render_tid = tid;
					apply_game_priority();
				});
			};
			cb.haptics = [this](const ipc::haptics_msg & h) {
				QMetaObject::invokeMethod(this, [this, h] {
					vr_haptics[h.hand & 1] = h.amplitude;
					if (current && current->video)
						current->video->set_vibration(vr_haptics[0], vr_haptics[1]);
				});
			};
			vr = std::make_unique<vr_server>(vr_render_node(), cb);
#ifdef _WIN32
			info(QString("[vr-ipc] OpenXR runtime pipe ready: %1").arg(QString::fromStdWString(ipc::pipe_name())));
#else
			info(QString("[vr-ipc] OpenXR runtime socket ready: %1").arg(QString::fromStdString(ipc::socket_path())));
#endif
			if (vr_render_node() != default_render_node())
				info(QString("[vr] VR apps render and encode on %1 (discrete GPU; the display runs on %2)")
				             .arg(QString::fromStdString(vr_render_node()), QString::fromStdString(default_render_node())));
		}
		catch (const std::exception & e)
		{
			info(QString("[vr-ipc] cannot start the runtime socket: %1").arg(e.what()));
			return;
		}
	}
	session * sess = current.get();
	bool active = sess && sess->streamer.value("StreamingSource").toInt() == 1;
	fov_port fov[2];
	float ipd = 0.063f;
	int hmd = 0;
	if (sess)
	{
		std::lock_guard lock(sess->vr_tracking->m);
		fov[0] = sess->vr_tracking->fov[0];
		fov[1] = sess->vr_tracking->fov[1];
		ipd = sess->vr_tracking->ipd;
		hmd = sess->mobile.value("HmdType").toInt();
	}
	vr->set_display(sess != nullptr, active, ipd, fov, hmd);
	ipc::render_settings settings = options.render;
	if (sess) {
		settings.gamma = float(std::clamp(sess->user.value("Gamma").toDouble(1), 0.1, 3.));
		settings.features = uint32_t(sess->mobile.value("Features").toInt());
		settings.emulate_trackers = sess->user.value("EmulateTrackers").toBool();
		settings.emulate_index = sess->user.value("EmulateIndexControllers").toBool();
		settings.spacewarp = sess->mobile.value("UseSpacewarp").toBool();
		if (bool(settings.spacewarp) != std::exchange(sess->spacewarp_logged, bool(settings.spacewarp)))
			info(QString("[vr] synchronous spacewarp %1").arg(settings.spacewarp ? "on: games render at half rate, the headset fills in"
			                                                                    : "off"));
	}
	vr->set_settings(settings);
	// the OpenXR app may start before the headset enters VR mode: size the shared buffers now
	if (sess)
	{
		vr_stream_params p = vr_params(sess);
		vr->set_stream(p.eye_width, p.eye_height, p.encoded_width(), p.encoded_height(), p.fps, is_10bit(p.codec), p.full_range);
		vr->set_foveation(p.fovea_width, p.fovea_height);
	}
}

vr_stream_params native_engine::vr_params(session * sess) const
{
	int hmd = sess->mobile.value("HmdType").toInt(1000);
	vr_stream_params p;
	p.fps = sess->user.value("VRFramerate").toInt(72);
	vr_eye_resolution(hmd, sess->user.value("VRGraphicsQuality").toInt(1), p.eye_width, p.eye_height);
	switch (options.codec_id)
	{
		case 1: p.codec = video_codec::h264; break;
		case 5: p.codec = video_codec::h264_plus; break;
		case 2: p.codec = video_codec::hevc; break;
		case 10: p.codec = video_codec::av1; break;
		case 11: p.codec = video_codec::av1_10bit; break;
		default: p.codec = video_codec::hevc_10bit; break;
	}
	// headset VRBitrateLimit: lerp(10 Mbps, max for the headset), capped by MeasuredBandwidth (the headset's VRBitrate)
	double limit = sess->user.value("VRBitrateLimit").toDouble(0.5);
	int64_t wanted = int64_t(10e6 + (double(vr_max_bitrate(hmd, p.codec)) - 10e6) * limit);
	int64_t measured = sess->mobile.value("MeasuredBandwidth").toVariant().toLongLong();
	p.kbps = int(std::max<int64_t>((measured > 0 ? std::min(wanted, measured) : wanted) / 1000, 5000));
	p.full_range = sess->user.value("IncreaseVideoNominalRange").toBool();
	p.vbaq = options.aq;
	p.pre_encode = options.two_pass;
	p.adaptive_speed = options.adaptive_speed;
	// foveated streaming: the headset enables it (eye-tracked headsets, SharedMobileSettings.FoveatedStreaming); the fovea
	// crop is the eye render size x "Fovea Size", in multiples of 64 like the Windows streamer - at least 384 x 384: AMD's
	// HEVC / AV1 encoders take no narrower frames (VAAPI constraints: width 384-8192), small Fovea Sizes would fail there
	if (sess->mobile.value("FoveatedStreaming").toBool())
	{
		auto crop = [&](int eye) { return std::min(std::max((int(eye * options.fovea_size) + 63) / 64 * 64, 384), eye / 16 * 16); };
		p.fovea_width = crop(p.eye_width);
		p.periphery_scale = options.periphery_scale;
		p.fovea_height = crop(p.eye_height);
	}
	return p;
}

void native_engine::start_vr(bool force)
{
	session * sess = current.get();
	vr_stream_params p = vr_params(sess);
	if (sess->video && sess->video->running() && !force && sess->vr_external == (vr && vr->has_client()) && sess->vr_params_signature == std::format("{}/{}x{}/{}/{}/{}/{}/{}/{}/{}x{}/{}", p.fps, p.eye_width, p.eye_height, p.kbps, int(p.codec), p.pre_encode, p.vbaq, p.full_range, p.adaptive_speed, p.fovea_width, p.fovea_height, p.periphery_scale))
		return;
	sess->vr_external = vr && vr->has_client();
	sess->vr_params_signature = std::format("{}/{}x{}/{}/{}/{}/{}/{}/{}/{}x{}/{}", p.fps, p.eye_width, p.eye_height, p.kbps, int(p.codec), p.pre_encode, p.vbaq, p.full_range, p.adaptive_speed, p.fovea_width, p.fovea_height, p.periphery_scale);
	stop_video_source(sess);
	auto tracking = sess->vr_tracking;
	auto logger = [this](std::string line) { QMetaObject::invokeMethod(this, [this, line] { info(QString::fromStdString(line)); }); };
	vr_sender::callbacks cb;
	cb.log = logger;
	cb.pose = [tracking] {
		vr_render_info info;
		std::lock_guard lock(tracking->m);
		std::array<float, 4> q{0, 0, 0, 1};
		std::array<float, 3> pos{0, 1.6f, 0};
		if (tracking->frame.size() >= 148)
		{
			info.pose_index = read_le<int32_t>(tracking->frame.data());
			std::memcpy(q.data(), tracking->frame.data() + 120, 16);
			std::memcpy(pos.data(), tracking->frame.data() + 136, 12);
		}
		// eyes: head position -/+ half the IPD along the head's right axis
		auto [x, y, z, w] = q;
		float right[3] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)};
		float half = tracking->ipd / 2;
		for (int e = 0; e < 2; ++e)
		{
			float sign = e == 0 ? -1.f : 1.f;
			info.eye[e].orientation = q;
			for (int k = 0; k < 3; ++k)
				info.eye[e].position[k] = pos[k] + sign * half * right[k];
			info.fov[e] = tracking->fov[e];
		}
		return info;
	};
	cb.pose_age_ms = [tracking](int32_t index) {
		std::lock_guard lock(tracking->m);
		auto [i, ns] = tracking->arrivals[size_t(uint32_t(index)) % tracking->arrivals.size()];
		if (i != index || !ns)
			return -1.0;
		return double(std::chrono::steady_clock::now().time_since_epoch().count() - ns) / 1e6;
	};
	cb.active_codec = [this, sess](video_codec c) {
		QMetaObject::invokeMethod(this, [this, sess, c] {
			if (current.get() == sess)
				send_streamer_settings({{"ActiveCodec", int(c)}}, false);
		});
	};
	bool external = vr && vr->has_client();
	if (!external && !vr_test_pattern())
	{
		// VR mode without an app. While a game is starting (Play in the headset's Games tab switches to VR right away)
		// or between two apps, stay in VR - black, like Windows while a game loads - instead of flipping to the
		// desktop and back. Otherwise (e.g. the headset still thought an app was running): back to the desktop.
		if (vr_linger || game_launch_pending())
			return;
		send_streamer_settings({{"HasVR", false}, {"StreamingSource", 0}}, false);
		update_video(true);
		return;
	}
	if (vr)
	{
		vr->set_stream(p.eye_width, p.eye_height, p.encoded_width(), p.encoded_height(), p.fps, is_10bit(p.codec), p.full_range);
		vr->set_foveation(p.fovea_width, p.fovea_height);
	}
	auto sender = std::make_shared<vr_sender>(*sess->video_out, p, cb, !external);
	sess->video = sender;
	if (external)
		vr->set_sink(sender);
	emit_event("video", {{"mode", "vr"}, {"fps", p.fps}, {"kbps", p.kbps}, {"codec", codec_name(p.codec)},
	                     {"source", external ? vr_app : QString("test pattern")}});
}

void native_engine::schedule_video_restart(int delay_ms)
{
	if (!current || current->restart_pending)
		return;
	current->restart_pending = true;
	session * sess = current.get();
	QTimer::singleShot(delay_ms, this, [this, sess] {
		if (current.get() != sess)
			return;
		sess->restart_pending = false;
		update_video(true);
	});
}

void native_engine::update_monitor_flags(int display_count)
{
	session * sess = current.get();
	int limit = std::min(max_monitors(sess->mobile.value("HmdType").toInt(-1)), 4);
	int count = std::clamp(monitor_count, 1, std::max(display_count, 1));
	bool can_grow = display_count > count || (sess->displays && sess->displays->can_create_virtual());
	QJsonObject flags{{"CanSwitchMonitor", display_count > 1},
	                  {"CanAddMonitor", count < limit && can_grow},
	                  {"CanRemoveMonitor", count > 1}};
	if (flags == sess->monitor_flags)
		return;
	sess->monitor_flags = flags;
	send_streamer_settings(flags, false);
}

void native_engine::on_monitor_message(uint8_t type, const bytes & payload)
{
	session * sess = current.get();
	if (!sess || !sess->displays || !sess->displays->ready())
		return;
	std::vector<display_info> all = sess->displays->displays();
	int n = int(all.size());
	int count = std::clamp(monitor_count, 1, std::max(n, 1));
	if (type == uint8_t(message_type::switch_monitor))
	{
		if (n < 2)
			return;
		sess->monitor_start = (sess->monitor_start + 1) % n;
		sess->has_switched_monitor = true;
		info(QString("[monitors] switched: main screen is now %1").arg(QString::fromStdString(all[size_t(sess->monitor_start)].name)));
	}
	else if (type == uint8_t(message_type::add_monitor))
	{
		if (!sess->monitor_flags.value("CanAddMonitor").toBool())
			return;
		if (n <= count) // every monitor is shown already: add a virtual one (Windows: IVirtualMonitor.Add)
		{
			QString error;
			auto mode = virtual_mode_for(sess);
			auto name = sess->displays->add_virtual(mode, &error);
			if (!name)
				return info("[monitors] cannot add a virtual monitor: " + error);
			info(QString("[monitors] virtual monitor %1 (%2x%3 @%4 Hz)")
			             .arg(name->empty() ? QString("requested") : QString::fromStdString(*name) + " added")
			             .arg(mode.width).arg(mode.height).arg(mode.refresh_mhz / 1000.0, 0, 'f', 1));
		}
		monitor_count = count + 1;
		save_engine_value("monitor_count", monitor_count);
	}
	else if (type == uint8_t(message_type::remove_monitor))
	{
		if (count <= 1)
			return;
		if (sess->displays->virtual_count() > (sess->extra_display ? 1 : 0) && sess->displays->remove_virtual())
			info("[monitors] virtual monitor removed");
		monitor_count = count - 1;
		save_engine_value("monitor_count", monitor_count);
	}
	else if (type == uint8_t(message_type::set_monitor_portrait))
	{
		if (payload.size() < 5)
			return;
		int index = read_be32(payload.data());
		bool portrait = payload[4];
		if (index < 0 || index >= int(sess->streamed.size()))
			return;
		QString error;
		const display_info & d = sess->streamed[size_t(index)];
		if (!sess->displays->set_portrait(d, portrait, &error))
			return info("[monitors] portrait mode failed: " + error);
		info(QString("[monitors] %1 -> %2").arg(QString::fromStdString(d.name), portrait ? "portrait" : "landscape"));
		stop_video_source(sess);
		schedule_video_restart(1000); // let the compositor apply the new geometry
		return;
	}
	update_video(true);
}

display_manager::virtual_mode native_engine::virtual_mode_for(session * sess)
{
	// like the main monitor (Windows' virtual monitor driver mirrors the primary's mode); size overridable
	auto mode = sess->displays->main_monitor_mode();
	if (options.virtual_width > 0 && options.virtual_height > 0)
	{
		mode.width = options.virtual_width;
		mode.height = options.virtual_height;
	}
	return mode;
}

void native_engine::sync_extra_display()
{
	// SharedUserSettings.ExtraDisplay: a virtual monitor for the headset, shown as the main screen
	session * sess = current.get();
	if (!sess || !sess->displays || !sess->displays->ready() || !sess->displays->can_create_virtual())
		return;
	bool wanted = sess->user.value("ExtraDisplay").toBool();
	if (wanted == sess->extra_display)
		return;
	if (wanted)
	{
		QString error;
		auto mode = virtual_mode_for(sess);
		auto name = sess->displays->add_virtual(mode, &error);
		if (!name)
		{
			sess->extra_display = true; // don't retry on every restart
			return info("[monitors] cannot create the extra display: " + error);
		}
		info(QString("[monitors] extra display %1 (%2x%3 @%4 Hz)")
		             .arg(name->empty() ? QString("requested") : QString::fromStdString(*name) + " created")
		             .arg(mode.width).arg(mode.height).arg(mode.refresh_mhz / 1000.0, 0, 'f', 1));
		if (!sess->has_switched_monitor)
		{
			auto all = sess->displays->displays();
			for (size_t i = 0; i < all.size(); ++i)
				if (all[i].name == *name)
					sess->monitor_start = int(i);
		}
	}
	else
	{
		sess->has_switched_monitor = false;
		sess->monitor_start = 0;
		sess->displays->remove_virtual();
		info("[monitors] extra display removed");
	}
	sess->extra_display = wanted;
	if (sess->video) // called from the settings handler: restart with the new layout
		schedule_video_restart(500);
}

#ifdef _WIN32
void native_engine::map_touch_outputs(int) {} // touch injection already targets the right screen
#else
void native_engine::map_touch_outputs(int attempt)
{
	// KWin maps a touchscreen onto one output (org.kde.KWin.InputDevice.outputName); udev + KWin need a moment
	// to pick up a new uinput device, so retry for a few seconds
	session * sess = current.get();
	if (!sess || !sess->injector)
		return;
	const input_layout & l = sess->injector->layout();
	std::vector<std::string> nodes = sess->injector->touch_devnodes();
	bool all_done = true;
	for (size_t i = 0; i < nodes.size() && i < l.screens.size(); ++i)
	{
		if (nodes[i].empty() || l.screens[i].output.empty())
			continue;
		QString sys = QString::fromStdString(nodes[i]).section('/', -1); // eventN
		QDBusMessage m = QDBusMessage::createMethodCall("org.kde.KWin", "/org/kde/KWin/InputDevice/" + sys,
		                                                "org.freedesktop.DBus.Properties", "Set");
		m.setArguments({"org.kde.KWin.InputDevice", "outputName",
		                QVariant::fromValue(QDBusVariant(QString::fromStdString(l.screens[i].output)))});
		QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 500);
		if (r.type() == QDBusMessage::ErrorMessage)
			all_done = false;
	}
	if (all_done)
		return info(QString("[input] touchscreens mapped to their monitors (%1)").arg(nodes.size()));
	if (attempt < 15)
		QTimer::singleShot(200, this, [this, attempt, sess, l] {
			if (current.get() == sess && sess->injector && sess->injector->layout() == l)
				map_touch_outputs(attempt + 1);
		});
	else
		info("[input] could not map the touchscreens to their monitors (KWin D-Bus)");
}
#endif

// ---------------------------------------------------------------------------------------------------------------
// Games tab

// Games payload: i16 count, {u8 platform, u64 app id, bool running}
static bytes games_list(const std::vector<game_entry> & games, const std::set<game_id> & running)
{
	net_writer w;
	w.i16(int16_t(std::min<size_t>(games.size(), 32767)));
	for (const auto & g: games)
		w.u8(uint8_t(g.platform)).be(g.app_id).u8(running.count(g.id()) ? 1 : 0);
	return w.b;
}

const game_entry * native_engine::find_game(game_id id) const
{
	for (const auto & g: games)
		if (g.id() == id)
			return &g;
	return nullptr;
}

void native_engine::on_games_message(uint8_t type, const bytes & payload)
{
	net_reader r(payload);
	switch (message_type(type))
	{
		case message_type::request_games:
			send_games();
			break;
		case message_type::request_game_thumbnails:
		{
			int n = r.i16();
			std::vector<game_entry> wanted;
			for (int i = 0; i < n && r.ok(); ++i)
			{
				auto platform = game_platform(r.u8());
				uint64_t id = r.be<uint64_t>();
				if (const game_entry * g = find_game({platform, id}); g && r.ok())
					wanted.push_back(*g);
			}
			session * sess = current.get();
			QPointer<native_engine> self(this);
			QThreadPool::globalInstance()->start([self, sess, wanted] {
				for (const auto & g: wanted)
				{
					QByteArray png = game_thumbnail(g);
					if (!self)
						return;
					net_writer w;
					w.u8(uint8_t(g.platform)).be(g.app_id).i32(int32_t(png.size()));
					w.raw(reinterpret_cast<const uint8_t *>(png.constData()), size_t(png.size()));
					QMetaObject::invokeMethod(self, [self, sess, b = std::move(w.b)] {
						if (self && self->current.get() == sess)
							self->send_message(message_type::game_thumbnail, b);
					});
				}
			});
			break;
		}
		case message_type::launch_game:
		{
			auto platform = game_platform(r.u8());
			uint64_t id = r.be<uint64_t>();
			if (r.ok())
				launch_game({platform, id});
			break;
		}
		case message_type::exit_game:
		{
			auto platform = game_platform(r.u8());
			uint64_t id = r.be<uint64_t>();
			if (r.ok())
				exit_game({platform, id});
			break;
		}
		default:
			break;
	}
}

// scans in the background (appinfo.vdf is several MB), then answers with the whole list
void native_engine::send_games()
{
	if (!games_timer)
	{
		games_timer = new QTimer(this);
		games_timer->setInterval(2000);
		connect(games_timer, &QTimer::timeout, this, [this] { poll_games(true); });
		games_timer->start();
	}
	if (games_scanning)
		return;
	games_scanning = true;
	QPointer<native_engine> self(this);
	QThreadPool::globalInstance()->start([self] {
		auto found = scan_games();
		QMetaObject::invokeMethod(self, [self, found = std::move(found)]() mutable {
			if (!self)
				return;
			self->games_scanning = false;
			self->games = std::move(found);
			int oculus = 0;
			for (const auto & g: self->games)
				oculus += g.platform == game_platform::oculus_rift;
			self->info(QString("[games] %1 RiftLift, %2 Steam VR").arg(oculus).arg(self->games.size() - size_t(oculus)));
			self->sync_dxvk();
			self->poll_games(false); // fresh running state; the list below carries it
			self->send_message(message_type::games, games_list(self->games, self->games_running));
		});
	});
}

#ifdef _WIN32
void native_engine::sync_dxvk()
{
	if (games.empty())
	{
		if (!games_scanning && (options.dxvk_default || !options.dxvk_games.isEmpty()))
		{
			games_scanning = true;
			QPointer<native_engine> self(this);
			QThreadPool::globalInstance()->start([self] {
				auto found = scan_games();
				QMetaObject::invokeMethod(self, [self, found = std::move(found)]() mutable {
					if (!self)
						return;
					self->games_scanning = false;
					self->games = std::move(found);
					self->sync_dxvk();
				});
			});
		}
		return;
	}
	// file copies off the engine thread; one pass over every game (placing or removing as the settings say)
	std::vector<std::pair<game_entry, bool>> work;
	for (const auto & g: games)
	{
		const bool want = dxvk::wanted(g, options.dxvk_default, options.dxvk_games);
		work.push_back({g, want});
	}
	QPointer<native_engine> self(this);
	QThreadPool::globalInstance()->start([self, work = std::move(work)] {
		auto log = [self](QString line) {
			QMetaObject::invokeMethod(self, [self, line] {
				if (self)
					self->info(line);
			});
		};
		for (const auto & [g, want]: work)
			if (want || dxvk::installed(g))
				dxvk::apply(g, want, log);
	});
}
#else
void native_engine::sync_dxvk() {} // Linux: Proton brings DXVK
#endif

void native_engine::launch_game(game_id id)
{
	const game_entry * g = find_game(id);
	auto fail = [&](const QString & why) {
		info("[games] " + why);
		send_message(message_type::game_exited, net_writer{}.u8(uint8_t(id.first)).be(id.second).b);
	};
	if (!g)
		return fail(QString("launch: unknown game %1").arg(qulonglong(id.second)));
	if (process_table().running(*g))
		return info(QString("[games] %1 is already running").arg(g->name));

	QProcess p;
#ifdef _WIN32
	QString log_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
#else
	QString log_dir = (qEnvironmentVariableIsEmpty("XDG_STATE_HOME") ? QDir::homePath() + "/.local/state" : qEnvironmentVariable("XDG_STATE_HOME")) + "/opendesk";
#endif
	QDir().mkpath(log_dir);
	p.setStandardOutputFile(log_dir + "/game-launch.log", QIODevice::Append);
	p.setStandardErrorFile(log_dir + "/game-launch.log", QIODevice::Append);
	qint64 pid = 0;
#ifdef _WIN32
	if (g->platform == game_platform::oculus_rift)
	{
		// The original streamer starts OculusClient (the store's runtime UI) first when it is not running: games
		// query it for the library and entitlements, and some refuse to start without it.
		if (!platform::any_process_has("OculusClient.exe"))
		{
			QString client = oculus_client_executable();
			if (QFileInfo::exists(client))
			{
				QProcess c;
				c.setProgram(client);
				c.setWorkingDirectory(QFileInfo(client).absolutePath());
				c.startDetached();
			}
		}
		if (g->launch_exe.isEmpty() || !QFileInfo::exists(g->launch_exe))
			return fail(QString("launch: %1 is not installed").arg(g->name));
		p.setProgram(g->launch_exe);
		p.setWorkingDirectory(g->install_dir);
		if (!g->launch_args.isEmpty())
			p.setArguments(QProcess::splitCommand(g->launch_args));
	}
	else
	{
		// Steam handles the vr://-style launch URL itself, including starting the client when needed.
		if (!QDesktopServices::openUrl(QUrl(QString("steam://launch/%1/vr").arg(qulonglong(g->app_id)))))
			return fail("launch: the Steam client is not available");
		info(QString("[games] launching %1 (steam://launch/%2/vr)").arg(g->name).arg(qulonglong(g->app_id)));
		games_launching[id] = QDateTime::currentMSecsSinceEpoch();
		vr_wait_until_ms = QDateTime::currentMSecsSinceEpoch() + 120'000;
		games_running.insert(id);
		return;
	}
#else
	if (g->platform == game_platform::oculus_rift)
	{
		QString exe = riftlift_executable();
		if (exe.isEmpty())
			return fail("launch: riftlift is not installed");
		// RiftLift prepares Proton + its Rift bridge and targets the active OpenXR runtime (ours while we run)
		p.setProgram(exe);
		p.setArguments({"launch", g->riftlift_slug});
	}
	else
	{
		QString steam = QStandardPaths::findExecutable("steam");
		p.setProgram(steam.isEmpty() ? "xdg-open" : steam);
		p.setArguments({QString("steam://launch/%1/vr").arg(qulonglong(g->app_id))});
	}
#endif
	if (!p.startDetached(&pid))
		return fail(QString("launch: could not start %1").arg(p.program()));
	info(QString("[games] launching %1 (%2 %3)").arg(g->name, p.program(), p.arguments().join(' ')));
#ifndef _WIN32
	if (g->platform == game_platform::oculus_rift)
		riftlift_pids[id] = pid;
#endif
	games_launching[id] = QDateTime::currentMSecsSinceEpoch();
	vr_wait_until_ms = QDateTime::currentMSecsSinceEpoch() + 120'000;
	games_running.insert(id); // the headset already shows it as running
}

// like closing the game's window: SIGINT to RiftLift (it tears its Proton tree down), SIGTERM to the game's processes,
// SIGKILL to whatever is left after 5 s
void native_engine::exit_game(game_id id)
{
	if (id.second == 0)
	{
		// the headset's VR menu "Exit game" (LeftPanel): whatever runs; Windows closes the current VR app
		std::set<game_id> running = games_running;
		for (const auto & r: running)
			if (r.second)
				exit_game(r);
		if (running.empty() && vr && vr->client_pid() > 0)
		{
			info(QString("[games] closing the OpenXR app %1").arg(vr_app));
			platform::terminate_process(vr->client_pid(), false);
		}
		return;
	}
	const game_entry * g = find_game(id);
	games_launching.erase(id);
	if (!g)
		return;
	info(QString("[games] exiting %1").arg(g->name));
	riftlift_pids.erase(id);
	process_table procs;
	process_table::markers markers = game_markers[id];
	markers.merge(procs.launch_markers(*g));
	std::vector<int> launchers = procs.launchers(*g);
	for (int pid: launchers)
		platform::terminate_process(pid, false); // RiftLift tears its Proton tree down itself on SIGTERM
	process_table::snapshot tree = procs.take(*g, markers);
	if (launchers.empty())
		for (auto [pid, _]: tree)
			platform::terminate_process(pid, false);
	// then whatever is left of this launch: terminate after 5 s, force after 10 s
	for (int step: {1, 2})
		QTimer::singleShot(5000 * step, this, [tree, step, markers] {
			for (int pid: process_table().remaining(tree, markers))
				platform::terminate_process(pid, step == 2);
		});
}

// running state: GameExited when a game's processes are gone, a fresh list when one started elsewhere
void native_engine::poll_games(bool announce)
{
	const bool pending_before = game_launch_pending();
	bool any_exited = false;
	if (games.empty())
		return;
	process_table procs;
	qint64 now = QDateTime::currentMSecsSinceEpoch();
	std::set<game_id> running;
	for (const auto & g: games)
	{
		game_id id = g.id();
		game_markers[id].merge(procs.launch_markers(g));
		if (procs.running(g, game_markers[id]))
		{
			running.insert(id);
			games_launching.erase(id);
		}
		else if (auto it = games_launching.find(id); it != games_launching.end())
		{
			if (now - it->second < 90'000) // Steam / Proton can take a while before the game process exists
				running.insert(id);
			else
			{
				info(QString("[games] %1 did not start").arg(g.name));
				games_launching.erase(it);
			}
		}
	}
	for (auto it = game_markers.begin(); it != game_markers.end();)
		it = running.count(it->first) ? std::next(it) : game_markers.erase(it);
	for (auto it = riftlift_pids.begin(); it != riftlift_pids.end();)
		it = running.count(it->first) ? std::next(it) : riftlift_pids.erase(it);
	bool started = false;
	for (const auto & id: games_running)
		if (!running.count(id))
		{
			if (const game_entry * g = find_game(id))
				info(QString("[games] %1 exited").arg(g->name));
			send_message(message_type::game_exited, net_writer{}.u8(uint8_t(id.first)).be(id.second).b);
			any_exited = true;
		}
	for (const auto & id: running)
		started |= !games_running.count(id);
	games_running = std::move(running);
	if (any_exited && games_running.empty())
		vr_wait_until_ms = 0; // nothing left to wait for
	if (pending_before != game_launch_pending() || any_exited)
		sync_vr_state(); // a launch ended (started, failed or quit): VR only if an app renders
	if (started && announce) // started outside the headset (desktop, RiftLift app): the list carries the flags
		send_message(message_type::games, games_list(games, games_running));
}

void native_engine::send_message(message_type type, const bytes & payload)
{
	if (!current || !current->messaging || !current->writer)
		return;
	bytes chunk = current->writer->encode(encode_net_message(type, payload.data(), payload.size()));
	current->messaging->write(reinterpret_cast<const char *>(chunk.data()), qint64(chunk.size()));
}

void native_engine::send_data(session * sess, const uint8_t * p, size_t n)
{
	if (!sess->data)
		return;
	if (sess->data_writer)
	{
		bytes c = sess->data_writer->encode(p, n);
		sess->data->write(reinterpret_cast<const char *>(c.data()), qint64(c.size()));
	}
	else
		sess->data->write(reinterpret_cast<const char *>(p), qint64(n));
}

void native_engine::start_download(session * sess, const std::string & path)
{
	std::string root = (options.videos_root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
	                                                  : options.videos_root)
	                           .toStdString();
	sess->download.reset();
	auto full = resolve_video_path(root, path);
	auto file = full ? std::make_unique<QFile>(QString::fromStdString(*full)) : nullptr;
	if (!file || !file->open(QIODevice::ReadOnly))
	{
		const uint8_t failure = 4; // DataType.OpenFailure
		send_data(sess, &failure, 1);
		info(QString("[videos] download of %1: cannot open it").arg(QString::fromStdString(path)));
		return;
	}
	// DataType.FullVideo, i64 BE size, the bytes
	bytes header{0};
	const uint64_t size = uint64_t(file->size());
	for (int i = 7; i >= 0; --i)
		header.push_back(uint8_t(size >> (8 * i)));
	send_data(sess, header.data(), header.size());
	sess->download_left = file->size();
	sess->download = std::move(file);
	info(QString("[videos] downloading %1 (%2 MB) to the headset").arg(QString::fromStdString(path)).arg(size >> 20));
	pump_download(sess);
}

void native_engine::pump_download(session * sess)
{
	if (!sess->download || !sess->data)
		return;
	QByteArray chunk(1 << 20, Qt::Uninitialized);
	while (sess->download_left > 0 && sess->data->bytesToWrite() < (8 << 20)) // a few MB in flight, the rest on demand
	{
		qint64 n = sess->download->read(chunk.data(), std::min<qint64>(chunk.size(), sess->download_left));
		if (n <= 0)
			break;
		send_data(sess, reinterpret_cast<const uint8_t *>(chunk.constData()), size_t(n));
		sess->download_left -= n;
	}
	if (sess->download_left <= 0)
	{
		info(QString("[videos] download of %1 sent").arg(sess->download->fileName()));
		sess->download.reset();
	}
}

void native_engine::on_video_message(uint8_t type, const bytes & payload)
{
	session * sess = current.get();
	net_reader r(payload);
	std::string root = (options.videos_root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
	                                                  : options.videos_root)
	                           .toStdString();
	switch (message_type(type))
	{
		case message_type::request_videos:
		{
			std::string path = r.str();
			send_message(message_type::videos, list_videos(root, path));
			break;
		}
		case message_type::request_video_thumbnails:
		{
			int n = r.i16();
			std::vector<std::string> files;
			for (int i = 0; i < n && r.ok(); ++i)
				files.push_back(r.str());
			QPointer<native_engine> self(this);
			QThreadPool::globalInstance()->start([self, sess, files, root] {
				for (const auto & f: files)
				{
					auto full = resolve_video_path(root, f);
					auto png = full ? video_thumbnail_png(*full) : std::nullopt;
					if (!png || !self)
						continue;
					net_writer w;
					w.str(f).i32(int32_t(png->size())).raw(png->data(), png->size());
					QMetaObject::invokeMethod(self, [self, sess, b = std::move(w.b)] {
						if (self && self->current.get() == sess)
							self->send_message(message_type::video_thumbnail, b);
					});
				}
			});
			break;
		}
		case message_type::play_video:
		{
			std::string path = r.str();
			int64_t start = r.i64();
			int audio = int8_t(r.u8());
			start_video(QString::fromStdString(path), start, audio);
			break;
		}
		case message_type::stop_video:
			stop_video(false);
			break;
		case message_type::download_video:
			start_download(sess, r.str());
			break;
		case message_type::cancel_download_video:
			if (sess->download)
			{
				info(QString("[videos] download cancelled with %1 MB left").arg(sess->download_left >> 20));
				sess->download.reset();
			}
			break;
		case message_type::seek_video:
			if (sess->player)
				sess->player->seek(r.i64());
			break;
		case message_type::set_audio_stream_index:
			if (sess->player)
				sess->player->set_audio_track(int8_t(r.u8()));
			break;
		case message_type::set_subtitle_track_id:
			if (sess->player)
				sess->player->set_subtitle_track(r.guid());
			break;
		case message_type::set_subtitle_offset_amount:
			if (sess->player)
				sess->player->set_subtitle_offset_ms(r.i64());
			break;
		default:
			break;
	}
}

void native_engine::start_video(const QString & path, int64_t start_us, int audio_index)
{
	session * sess = current.get();
	if (!sess || !sess->video_out)
		return;
	std::string root = (options.videos_root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
	                                                  : options.videos_root)
	                           .toStdString();
	auto full = resolve_video_path(root, path.toStdString());
	if (!full)
		return info("[videos] refusing a path outside the videos folder: " + path);
	stop_video_source(sess); // the desktop stops while the file plays
	sess->player.reset();
	auto logger = [this](std::string line) { QMetaObject::invokeMethod(this, [this, line] { info(QString::fromStdString(line)); }); };
	float speed = float(sess->mobile.value("VideoPlaybackSpeed").toDouble(1));
	try
	{
		sess->player = std::make_unique<file_player>(
		        *sess->video_out, sess->audio_out.get(), *full, audio_index, speed,
		        file_player::callbacks{logger, [this, sess] {
			                               QMetaObject::invokeMethod(this, [this, sess] {
				                               if (current.get() == sess && sess->player && !sess->player->running())
					                               stop_video(true);
			                               });
		                               }});
	}
	catch (const std::exception & e)
	{
		info(QString("[videos] cannot play %1: %2").arg(path, e.what()));
		send_message(message_type::video_stopped, net_writer{}.str(path.toStdString()).b);
		update_video(true);
		return;
	}
	sess->playing_path = path;
	if (sess->audio)
		sess->audio->set_paused(true);
	send_streamer_settings({{"StreamingSource", 2}}, false); // StreamingSource.Video
	sess->video_out->send(sess->player->video_format());
	net_writer at;
	at.u8(uint8_t(sess->player->audio_tracks().size()));
	for (const auto & t: sess->player->audio_tracks())
		at.str(t.name).u8(uint8_t(t.channels)).u8(uint8_t(int8_t(t.index)));
	send_message(message_type::audio_tracks, at.b);
	net_writer st;
	st.u8(uint8_t(sess->player->subtitle_tracks().size()));
	for (const auto & t: sess->player->subtitle_tracks())
		st.guid(t.id).str(t.name).str(t.language).i32(t.index);
	send_message(message_type::subtitle_tracks, st.b);
	if (sess->mobile.value("IsVideoPaused").toBool())
		sess->player->set_paused(true);
	sess->player->start(start_us);
	info(QString("[videos] playing %1").arg(path));
	emit_event("video", {{"mode", "file"}, {"path", path}});
}

void native_engine::stop_video(bool notify_stopped)
{
	session * sess = current.get();
	if (!sess || !sess->player)
		return;
	sess->player.reset();
	if (notify_stopped) // reached the end
		send_message(message_type::video_stopped, net_writer{}.str(sess->playing_path.toStdString()).b);
	info("[videos] stopped " + sess->playing_path);
	sess->playing_path.clear();
	if (sess->audio)
		sess->audio->set_paused(false);
	send_streamer_settings({{"StreamingSource", 0}}, false);
	update_video(true); // back to the desktop
}

void native_engine::save_engine_value(const QString & key, const QJsonValue & value)
{
	QFile f(config_path());
	QJsonObject o;
	if (f.open(QIODevice::ReadOnly))
		o = QJsonDocument::fromJson(f.readAll()).object();
	f.close();
	o[key] = value;
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(QJsonDocument(o).toJson());
}

pulse_client * native_engine::pulse_audio()
{
	if (!pulse)
	{
		try
		{
			pulse = std::make_unique<pulse_client>();
		}
		catch (const std::exception & e)
		{
			info(QString("[audio] no audio server: %1").arg(e.what()));
		}
	}
	return pulse.get();
}

void native_engine::setup_audio_output(session * sess)
{
	// StreamerSettings.AudioStreaming, as on Windows:
	//   0 "Computer only": nothing is sent
	//   1 "VR headset only": PC speakers silent - via a virtual sink made default ("Use virtual audio driver"),
	//     or by muting the real sink while capturing its monitor
	//   2 "VR headset & computer": capture what the PC plays
	if (!sess->audio_socket)
		return;
	std::string device = "@DEFAULT_MONITOR@";
	pulse_client * pa = sess->pulse;
	if (options.audio_mode == 0)
	{
		info("[audio] 'Computer only': not streaming desktop audio");
		return;
	}
	if (options.audio_mode == 1 && pa)
	{
		if (options.virtual_audio)
		{
			sess->audio_sink_module = pa->load_module("module-null-sink",
			                                          "sink_name=vd_audio rate=48000 channels=2 "
			                                          "sink_properties=device.description=Virtual_Desktop_Audio");
			if (sess->audio_sink_module)
			{
				sess->restore_sink = pa->default_sink();
				pa->set_default_sink("vd_audio");
				device = "vd_audio.monitor";
			}
		}
		else
		{
			std::string sink = pa->default_sink();
			if (pa->sink_muted(sink) == false)
			{
				pa->set_sink_mute(sink, true);
				sess->unmute_sink = sink;
			}
		}
	}
	sess->audio = std::make_unique<desktop_audio_sender>(*sess->audio_out, device, audio_enabled);
	sess->audio->set_paused(bool(sess->player));
	info(QString("[audio] %1: capturing %2 -> headset (s16le 48 kHz stereo, 5 ms)%3")
	             .arg(options.audio_mode == 1 ? "VR headset only" : "VR headset & computer", QString::fromStdString(device),
	                  sess->encrypted ? ", encrypted" : ""));
}

void native_engine::on_mic_packet(session * sess, const uint8_t * pcm, size_t size)
{
	if (!sess->user.value("MicPassthrough").toBool(true)) // headset: "Microphone passthrough" off
		return;
	if (!sess->mic)
	{
		if (pulse_client * pa = sess->pulse)
		{
			sess->mic_module = pa->load_module("module-null-sink",
			                                   "media.class=Audio/Source/Virtual sink_name=opendesk_mic channel_map=mono rate=48000 "
			                                   "sink_properties=device.description=Virtual_Desktop_Microphone");
			if (options.auto_select_mic && sess->mic_module)
			{
				sess->restore_source = pa->default_source();
				pa->set_default_source("opendesk_mic");
			}
		}
		sess->mic = std::make_unique<mic_player>(float(sess->user.value("MicVolume").toDouble(0.85)));
		info(QString("[audio] headset mic -> 'OpenDesk Microphone'%1").arg(options.auto_select_mic ? " (selected as default input)" : ""));
	}
	sess->mic->push(pcm, size);
}

void native_engine::update_gamepad(session * sess, const bytes & frame)
{
	constexpr size_t gamepad_offset = 80, model_offset = 112; // FrameData.InputState.GamepadState / GamepadModel
	if (frame.size() <= model_offset)
		return;
	if (!sess->user.value("EmulateGamepad").toBool(true)) // headset: "Emulate gamepad" off
	{
		if (sess->gamepad)
		{
			info("[input] virtual gamepad removed (gamepad emulation turned off)");
			sess->gamepad.reset();
		}
		return;
	}
	const uint8_t * state = frame.data() + gamepad_offset;
	gamepad_model model = options.gamepad_emulation == 1 ? gamepad_model::xbox360
	                      : options.gamepad_emulation == 2 ? gamepad_model::dualshock4
	                                                       : gamepad_model(frame[model_offset] == 1 ? 1 : 0);
	if (!sess->gamepad || sess->gamepad->model() != model)
	{
		if (!gamepad_state_active(state))
			return; // create the pad on first use, like the Windows streamer
		sess->gamepad.reset();
		try
		{
			sess->gamepad = std::make_unique<virtual_gamepad>(model, [this, sess](float strong, float weak) {
				QMetaObject::invokeMethod(this, [this, sess, strong, weak] {
					if (current.get() == sess && sess->video)
						sess->video->set_vibration(strong, weak);
				});
			});
			info(QString("[input] virtual %1 gamepad created").arg(model == gamepad_model::dualshock4 ? "DualShock 4" : "Xbox 360"));
		}
		catch (const std::exception & e)
		{
			info(QString("[input] gamepad emulation unavailable: %1").arg(e.what()));
			return;
		}
	}
	sess->gamepad->apply(state);
}

void native_engine::save_screenshot(const bytes & payload)
{
	// ScreenshotManager: NetworkBinaryWriter.Write(string) = i16 BE length + UTF-8, then i32 BE size + PNG bytes
	if (payload.size() < 6)
		return;
	size_t name_len = size_t(uint16_t(payload[0] << 8 | payload[1]));
	if (payload.size() < 2 + name_len + 4)
		return;
	QString name = QFileInfo(QString::fromUtf8(reinterpret_cast<const char *>(payload.data() + 2), qsizetype(name_len))).fileName();
	int32_t size = read_be32(payload.data() + 2 + name_len);
	const uint8_t * png = payload.data() + 2 + name_len + 4;
	if (size <= 0 || png + size > payload.data() + payload.size())
		return;
	QString dir = options.screenshots_dir.isEmpty()
	                      ? QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).filePath("OpenDesk")
	                      : options.screenshots_dir;
	QDir().mkpath(dir);
	if (name.isEmpty())
		name = "screenshot.png";
	QString path = QDir(dir).filePath(QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss_") + name);
	QFile f(path);
	if (f.open(QIODevice::WriteOnly) && f.write(reinterpret_cast<const char *>(png), size) == size)
	{
		info("[media] screenshot saved: " + path);
		emit_event("screenshot", {{"path", path}});
	}
}

void native_engine::save_portal_token(const QString & token)
{
	if (token == portal_restore_token)
		return;
	portal_restore_token = token;
	save_engine_value("portal_restore_token", token);
	info("[capture] screen-share permission saved (reused on the next start)");
}

void native_engine::on_drain_connection(QTcpServer * server, const char * name)
{
	while (QTcpSocket * s = server->nextPendingConnection())
	{
		info(QString("[%1] connection from %2 (not implemented in the native engine yet)").arg(name, host_of(s->peerAddress())));
		connect(s, &QTcpSocket::readyRead, s, [s] { s->readAll(); });
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
}
} // namespace od
