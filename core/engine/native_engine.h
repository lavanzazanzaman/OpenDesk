// Native (C++) streaming engine. Lives in its own thread; talks to the GUI with the same JSON-shaped events as
// engine/vd_streamer.py ("ready", "pairing_request", "selected", "connected", "disconnected", "settings", "video").
//
// Stage 1: discovery, messaging + settings sync, bandwidth probe, FrameData decode.
// Next: input injection + audio (stage 2), in-process desktop capture/encode (stage 3), VR (stage 4).
#pragma once

#ifdef _WIN32
#include "displays_win.h"
#else
#include "displays.h"
#endif
#include "games.h"
#include "protocol.h"
#include "vr_ipc.h"

#include <array>
#include <atomic>
#include <map>
#include <set>

#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <memory>

class QTcpServer;
class QTimer;
class QTcpSocket;
class QUdpSocket;


namespace od
{
struct engine_options
{
	int codec_id = 0;                          // StreamerSettings.PreferredCodec (VideoCodec wire value, 0 = Automatic)
	bool auto_adjust_bitrate = true;           // AutoAdjustBitrate: measure the link (bandwidth probe) at connection
	bool aq = true;                            // "Adaptive quantization" -> AMD VBAQ
	bool two_pass = false;                     // "2-Pass encoding" -> AMD pre-encode
	bool adaptive_speed = true;                // "Adaptive encoder speed" -> VR NVENC preset follows the load
	double periphery_scale = 0.5;              // "Periphery Resolution": foveated streaming's periphery, fraction of the render size
	double fovea_size = 0.25;                  // "Fovea Size": foveated streaming crop, fraction of the eye render size
	QString adapter_name;                      // GPU name for the headset's stats (AdapterName)
	bool adapter_is_amd = false;
	int gamepad_emulation = 0;                 // GamepadEmulation: 0 Automatic (headset's model), 1 Xbox, 2 DualShock
	int audio_mode = 1;                        // AudioStreaming: 0 computer only, 1 VR headset only, 2 both
	bool virtual_audio = true;                 // headset only: route through a "OpenDesk Audio" sink
	bool auto_select_mic = true;               // make "OpenDesk Microphone" the default input while in use
	bool encrypt_local = false;                // EncryptLocalTraffic: AesStream on data/video/audio too
	bool lock_on_disconnect = false;
	bool boost_priority = false;               // ADVANCED Boost game priority: the VR app's render thread at nice -10 (rtkit)
	QString screenshots_dir;
	QString videos_root;                       // MEDIA: videos folder (Videos tab library root)
	bool no_audio = false;
	ipc::render_settings render;
	bool touch = true;                        // upstream "Use touch input"
	float mouth_sensitivity = 2;              // face tracking -> VRChat (face_tracking::set_mouth_sensitivity)
	int desktop_width = 0, desktop_height = 0; // physical pixels of the streamed desktop (from the GUI)
	int screens = 1;                           // connected displays (from the GUI); 0 = nothing to capture
	int virtual_width = 0, virtual_height = 0; // Add monitor / Extra display size override (0 = like the main monitor)
	int probe_bytes = 5000000;                 // Windows: 5 MB with AutoAdjustBitrate, none without
	std::array<int, 4> reported_version = default_streamer_version; // discovery/compatibility version
};

class native_engine : public QObject
{
	Q_OBJECT

public:
	explicit native_engine(QObject * parent = nullptr);
	~native_engine() override;

public slots:
	void start(QStringList accounts, QJsonObject options);
	void set_accounts(QStringList accounts);
	void set_options(QJsonObject options);
	void headset_action(QString action); // BINDINGS: SwitchMonitor, ToggleVRMode, passthrough toggles, ...

#ifndef _WIN32
private slots:
	void on_keyboard_layout_changed(uint index); // KDE org.kde.KeyboardLayouts.layoutChanged
#endif

signals:
	void event(QJsonObject);
	void log(QString);

private:
	struct session;

	void on_discovery(QUdpSocket *);
	void on_messaging_connection();
	void on_data_connection();
	void on_audio_connection();
	void setup_audio_output(session *);
	void on_mic_packet(session *, const uint8_t * pcm, size_t size);
	void save_screenshot(const bytes & payload);
	void update_gamepad(session *, const bytes & frame_data);
	class pulse_client * pulse_audio();
	void on_video_connection();
	void update_video(bool force = false);
	void on_drain_connection(QTcpServer *, const char * name);
	// multi-monitor (SwitchMonitor / AddMonitor / RemoveMonitor / SetMonitorPortrait / ExtraDisplay)
	void on_monitor_message(uint8_t type, const bytes & payload);
	void sync_extra_display();
	display_manager::virtual_mode virtual_mode_for(session *);
	void update_monitor_flags(int display_count);
	void schedule_video_restart(int delay_ms);
	void start_vr(bool force);
#ifndef _WIN32
	struct vr_stream_params vr_params(session *) const;
#endif
	void stop_video_source(session *);
	static bool vr_test_pattern();
	bool vr_available() const; // VR enabled and an OpenXR app has a session (HasVR)
	void fetch_xrizer();
	void apply_game_priority(); // "Boost game priority" for the VR app's render thread
	int render_pid = 0, render_tid = 0, boosted_tid = 0, boosted_from_nice = 0;
	void sync_vr_state();      // HasVR / StreamingSource to the headset after an app started or quit
	void update_vr_server(); // creates/destroys the OpenXR runtime socket with the VR option; pushes display state
	// Videos tab (library + file playback)
	void on_video_message(uint8_t type, const bytes & payload);
	void start_video(const QString & path, int64_t start_us, int audio_index);
	void stop_video(bool notify_stopped);
	// Videos tab: Download (PC file -> headset over the data connection)
	void start_download(session *, const std::string & path);
	void pump_download(session *);
	void send_data(session *, const uint8_t *, size_t);
	// Games tab (RiftLift library + Steam VR apps)
	void on_games_message(uint8_t type, const bytes & payload);
	void send_games();
	void launch_game(game_id);
	void exit_game(game_id);
	void poll_games(bool announce);
	const game_entry * find_game(game_id) const;
	void send_message(message_type, const bytes & payload);
	std::optional<std::pair<aes_key, aes_iv>> channel_encryption(session *) const;
	void map_touch_outputs(int attempt = 0);
	void send_streamer_settings(const QJsonObject & values, bool reload);
	QString input_language() const; // current keyboard layout as the headset keyboard's layout name (InputLanguage)
	void emit_event(const char * name, QJsonObject fields = {});
	void info(const QString &);

	computer_info computer;
	QSet<QString> accounts;
	engine_options options;
	std::atomic<bool> audio_enabled{true};
	QString portal_restore_token;
	qint64 last_capture_restart_ms = 0;
	int quick_capture_restarts = 0;
	void save_portal_token(const QString &);
	void save_engine_value(const QString & key, const QJsonValue & value);
	int monitor_count = 0; // StreamerSettings.MonitorCount: screens streamed at once (0 = not chosen yet)
	QUdpSocket * discovery_v4 = nullptr;
	QUdpSocket * discovery_v6 = nullptr;
	QTcpServer * messaging_server = nullptr;
	QTcpServer * data_server = nullptr;
	QTcpServer * video_server = nullptr;
	QTcpServer * audio_server = nullptr;
	std::unique_ptr<class pulse_client> pulse; // declared before `current`: sessions restore audio routing through it
#ifndef _WIN32
	std::unique_ptr<class vr_server> vr; // before `current`: sessions' senders unregister from it
#endif
	std::unique_ptr<class face_tracking> face; // headset face/eye tracking -> VRChat (OSC)
	QString vr_app;
	bool vr_app_rendered = false; // the connected app rendered (the headset may be showing it)
	bool vr_linger = false; // an app just quit: stay in VR briefly in case the next one (the real game) follows
	qint64 vr_wait_until_ms = 0; // a game launched from the headset: its first frame expected until then
	bool game_launch_pending() const;
	bool runtime_registered = false;
	bool openvr_registered = false, openvr_warned = false, xrizer_fetch_started = false;
	float vr_haptics[2] = {0, 0};
	std::vector<game_entry> games;             // last scan
	bool games_scanning = false;
	std::set<game_id> games_running;           // as last told to the headset
	std::map<game_id, qint64> games_launching; // launched, processes not seen yet (ms since epoch)
	std::map<game_id, qint64> riftlift_pids;   // `riftlift launch` wrappers we started
	std::map<game_id, process_table::markers> game_markers; // RiftLift launch ids of running games
	QTimer * games_timer = nullptr;
	std::unique_ptr<session> current;
};
} // namespace od
