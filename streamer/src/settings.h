// Streamer settings, mirroring Upstream.Streamer.StreamerSettings (defaults from its [DefaultValue]s).
// Stored as JSON in ~/.config/OpenDesk/streamer.json.
#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

// Upstream.Interfaces.Platform (values are the wire values)
enum class Platform
{
	Oculus = 0,
	OculusQuest = 1,
	Vive = 2,
	Pico = 3,
	Google = 5,
	PlayForDream = 6,
	Apple = 7,
	Steam = 8,
};

struct PlatformInfo
{
	Platform platform;
	const char * name; // EnumDisplayName
	const char * icon; // resource name under :/assets/opendesk/
};

// Order and names as in the Windows "Add account" menu
const QList<PlatformInfo> & platforms();
QString platformName(int wire_value);
QString hmdTypeName(int hmd_type); // Xenko.VR.HmdType [EnumDisplayName]s

// Upstream.Net.VideoCodec
enum class VideoCodec
{
	Automatic = 0,
	H264 = 1,
	HEVC = 2,
	H264Plus = 5,
	HEVC10bit = 6,
	AV1 = 10,
	AV110bit = 11,
};

enum class GamepadEmulation
{
	Automatic,
	Xbox,
	Dualshock
};

enum class AudioStreaming
{
	Disabled,          // "Computer only"
	HeadsetOnly,       // "VR headset only"
	HeadsetAndComputer // "VR headset & computer"
};

// Upstream.Streamer.BindingAction
extern const QStringList bindingActions;

class StreamerSettings : public QObject
{
	Q_OBJECT

public:
	static StreamerSettings & instance();

	int selectedTab = 0;
	QMap<int, QStringList> accounts; // Platform wire value -> AccountIDs

	// OPTIONS
	VideoCodec preferredCodec = VideoCodec::Automatic;
	bool enableAQ = true;
	bool enable2Pass = false;
	bool adaptiveEncoderSpeed = true; // VR: faster NVENC presets while encoding can't keep up with the refresh rate
	int nvencPreset = 1;              // NVENC preset: 1 speed (P1), 4 balanced (P4), 6 quality (P6)
	int amdEncoderSpeed = 0;          // AMD (VAAPI) quality preset: 0 speed, 1 balanced, 2 quality
	bool dualGpu = false;             // one card runs the games, another encodes the stream
	QString gameGpu, streamingGpu;    // PCI addresses (render nodes can change between boots)
	QString singleGpu;                // not dualGpu: the card for VR games; empty = automatic (the main screen's)
	QString effectiveGameGpu() const { return dualGpu ? gameGpu : singleGpu; }
	QString effectiveStreamingGpu() const { return dualGpu ? streamingGpu : singleGpu; }
	GamepadEmulation gamepadEmulation = GamepadEmulation::Automatic;
	AudioStreaming audioStreaming = AudioStreaming::HeadsetOnly;
	bool useVirtualAudioDriver = true;
	bool encryptLocalTraffic = false;
	bool autoAdjustBitrate = true;
	QStringList startupApps; // commands run while a VR game is open
	bool autoUpdate = true; // AppImage: download new GitHub releases in the background
	bool startWithLogin = false; // Windows defaults to true; off here so we never add autostart silently
	bool startMinimizedInTray = false;
	bool useTouchInput = true;
	bool lockComputer = false;
	bool autoSelectMicrophone = true;
	bool showPairingRequests = true;
	bool checkSetupAtStartup = true; // Linux: firewall/permission check dialog

	// ADVANCED
	double horizontalFovTangent = 1.0;
	double verticalFovTangent = 1.0;
	double renderResolutionOverride = 1.0;
	double foveaSize = 0.25;
	int handPose = 0;            // controllers on Quest 3 / Pro: 0 automatic, 1 PC calibration, 2 the headset's own grip
	double peripheryScale = 0.5; // foveated streaming: periphery resolution (fraction of the eye render size)
	bool useSharpening = false;
	double sharpening = 0.7;
	bool useFovStencil = true;
	bool softFoveaEdge = false; // foveated streaming: fade the fovea into the periphery (off: the Windows streamer's edge)
	bool boostGamePriority = false;
	double mouthSensitivity = 2.0; // face tracking -> VRChat: mouth / jaw response (1 = raw headset weights)
	QString reportedStreamerVersion = "1.20.3"; // compatibility: lowest Windows version the headset accepts
	bool reportedVersionAuto = true; // headset apps 1.34.25+ send their version: report it back (older: the version above)

	// MEDIA
	QString screenshotsRootPath;
	QString videosRootPath;
	QStringList videoFolders;

	// BINDINGS: action -> QKeySequence::toString()
	QMap<QString, QString> bindings;

	bool hasValidAccountID() const;
	QStringList allAccountIDs() const;

	void load();
	void save();
	void resetStreaming();
	void resetVR();
	void resetAudioInput();
	void resetGeneral();
	void resetBindings();

signals:
	void changed();

private:
	StreamerSettings();
	QString path() const;
};
