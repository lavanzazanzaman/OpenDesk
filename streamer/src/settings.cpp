#include "settings.h"

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

const QList<PlatformInfo> & platforms()
{
	static const QList<PlatformInfo> list{
	        {Platform::OculusQuest, "Meta", "platform-meta.svg"},
	        {Platform::Pico, "Pico", "platform-pico.svg"},
	        {Platform::Vive, "Viveport", "platform-viveport.svg"},
	        {Platform::Google, "Google", "platform-google.svg"},
	        {Platform::PlayForDream, "Play for Dream", "platform-playfordream.svg"},
	        {Platform::Apple, "Apple", "platform-apple.svg"},
	        {Platform::Steam, "Steam", "platform-steam.svg"},
	};
	return list;
}

QString platformName(int wire_value)
{
	for (const auto & p: platforms())
		if (int(p.platform) == wire_value)
			return p.name;
	return wire_value == int(Platform::Oculus) ? "Oculus" : QString("Platform %1").arg(wire_value);
}

QString hmdTypeName(int hmd_type)
{
	static const QMap<int, QString> names{
	        {10, "iPhone"}, {20, "iPad"}, {25, "Apple TV"}, {30, "Android Phone"}, {40, "Android Tablet"},
	        {45, "Android TV"}, {50, "Android Desktop"}, {259, "Oculus Quest"}, {320, "Oculus Quest 2"},
	        {400, "Meta Quest Pro"}, {600, "Vive Focus 3"}, {650, "Vive XR Elite"}, {700, "Pico Neo 3"},
	        {701, "Pico Neo 3 Link"}, {800, "Pico 4"}, {1000, "Meta Quest 3"}, {1001, "Meta Quest 3S"},
	        {1010, "Pico 4 Ultra"}, {1100, "Samsung Galaxy XR"}, {1400, "Play for Dream MR"}, {1500, "Steam Frame"},
	        {1600, "XREAL Aura"}, {1700, "Pico Space Pro"}, {1710, "Meta VR Glasses"}};
	return names.value(hmd_type, QString("Unknown headset (%1)").arg(hmd_type));
}

const QStringList bindingActions{
        "SwitchMonitor",
        "ToggleVRMode",
        "ToggleVRPassthrough",
        "EnableVRPassthrough",
        "DisableVRPassthrough",
        "ToggleHandPassthrough",
        "ToggleDeskPassthrough",
        "TogglePerformanceOverlay",
        "ToggleFoveatedStreaming",
};

StreamerSettings & StreamerSettings::instance()
{
	static StreamerSettings s;
	return s;
}

StreamerSettings::StreamerSettings()
{
	QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	screenshotsRootPath = QDir(pictures).filePath("OpenDesk");
	videosRootPath = QDir(movies).filePath("OpenDesk");
	load();
}

QString StreamerSettings::path() const
{
	return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
	        .filePath("OpenDesk/streamer.json");
}

bool StreamerSettings::hasValidAccountID() const
{
	for (const auto & ids: accounts)
		if (!ids.isEmpty())
			return true;
	return false;
}

QStringList StreamerSettings::allAccountIDs() const
{
	QStringList all;
	for (const auto & ids: accounts)
		all += ids;
	return all;
}

// Reset to Defaults, per tab
void StreamerSettings::resetStreaming()
{
	preferredCodec = VideoCodec::Automatic;
	enableAQ = true;
	enable2Pass = false;
	adaptiveEncoderSpeed = true;
	nvencPreset = 1;
	amdEncoderSpeed = 0;
	dualGpu = false;
	gameGpu.clear();
	streamingGpu.clear();
	singleGpu.clear();
	encryptLocalTraffic = false;
	autoAdjustBitrate = true;
	reportedStreamerVersion = "1.20.3";
	reportedVersionAuto = true;
	save();
}

void StreamerSettings::resetVR()
{
	horizontalFovTangent = 1.0;
	verticalFovTangent = 1.0;
	renderResolutionOverride = 1.0;
	foveaSize = 0.25;
	handPose = 0;
	peripheryScale = 0.5;
	useSharpening = false;
	sharpening = 0.7;
	useFovStencil = true;
	softFoveaEdge = false;
	boostGamePriority = false;
	mouthSensitivity = 2.0;
	save();
}

void StreamerSettings::resetAudioInput()
{
	audioStreaming = AudioStreaming::HeadsetOnly;
	useVirtualAudioDriver = true;
	autoSelectMicrophone = true;
	gamepadEmulation = GamepadEmulation::Automatic;
	useTouchInput = true;
	save();
}

void StreamerSettings::resetGeneral()
{
	startupApps.clear();
	autoUpdate = true;
	startWithLogin = false;
	startMinimizedInTray = false;
	lockComputer = false;
	showPairingRequests = true;
	save();
}

void StreamerSettings::resetBindings()
{
	bindings.clear();
	save();
}

void StreamerSettings::load()
{
	QFile f(path());
	if (!f.open(QIODevice::ReadOnly))
		return;
	const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();

	auto b = [&](const char * k, bool & v) { if (o.contains(k)) v = o[k].toBool(); };
	auto d = [&](const char * k, double & v) { if (o.contains(k)) v = o[k].toDouble(); };
	auto s = [&](const char * k, QString & v) { if (o.contains(k)) v = o[k].toString(); };
	auto e = [&](const char * k, auto & v) { if (o.contains(k)) v = std::remove_reference_t<decltype(v)>(o[k].toInt()); };

	if (o.contains("SelectedTab"))
		selectedTab = o["SelectedTab"].toInt();
	accounts.clear();
	const QJsonObject acc = o["Accounts"].toObject();
	for (auto it = acc.begin(); it != acc.end(); ++it)
		for (const auto & id: it.value().toArray())
			accounts[it.key().toInt()] << id.toString();

	e("PreferredCodec", preferredCodec);
	b("EnableAQ", enableAQ);
	b("Enable2Pass", enable2Pass);
	b("AdaptiveEncoderSpeed", adaptiveEncoderSpeed);
	if (o.contains("NvencPreset"))
	{
		const int v = o["NvencPreset"].toInt(1); // (older versions offered more steps)
		nvencPreset = v <= 2 ? 1 : v <= 5 ? 4 : 6;
	}
	if (o.contains("AmdEncoderSpeed"))
		amdEncoderSpeed = o["AmdEncoderSpeed"].toInt();
	b("DualGpu", dualGpu);
	s("GameGpu", gameGpu);
	s("StreamingGpu", streamingGpu);
	s("SingleGpu", singleGpu);
	e("GamepadEmulation", gamepadEmulation);
	e("AudioStreaming", audioStreaming);
	b("UseVirtualAudioDriver", useVirtualAudioDriver);
	b("EncryptLocalTraffic", encryptLocalTraffic);
	b("AutoAdjustBitrate", autoAdjustBitrate);
	b("AutoUpdate", autoUpdate);
	b("StartWithLogin", startWithLogin);
	b("StartMinimizedInTray", startMinimizedInTray);
	b("UseTouchInput", useTouchInput);
	b("LockComputer", lockComputer);
	b("AutoSelectMicrophone", autoSelectMicrophone);
	b("ShowPairingRequests", showPairingRequests);
	b("CheckSetupAtStartup", checkSetupAtStartup);
	d("HorizontalFovTangent", horizontalFovTangent);
	d("VerticalFovTangent", verticalFovTangent);
	d("RenderResolutionOverride", renderResolutionOverride);
	d("FoveaSize", foveaSize);
	foveaSize = std::clamp(foveaSize, 0.15, 0.5); // Windows: 15-35% (default 25%); up to 50% here
	if (o.contains("HandPose"))
		handPose = std::clamp(o["HandPose"].toInt(), 0, 2);
	d("PeripheryScale", peripheryScale);
	b("UseSharpening", useSharpening);
	d("Sharpening", sharpening);
	b("UseFovStencil", useFovStencil);
	b("SoftFoveaEdge", softFoveaEdge);
	b("BoostGamePriority", boostGamePriority);
	d("MouthSensitivity", mouthSensitivity);
	s("ReportedStreamerVersion", reportedStreamerVersion);
	b("ReportedVersionAuto", reportedVersionAuto);
	if (reportedStreamerVersion.isEmpty())
		reportedStreamerVersion = "1.20.3";
	s("ScreenshotsRootPath", screenshotsRootPath);
	s("VideosRootPath", videosRootPath);
	if (o.contains("StartupApps"))
	{
		startupApps.clear();
		for (const auto & v: o["StartupApps"].toArray())
			startupApps << v.toString();
	}
	if (o.contains("VideoFolders"))
	{
		videoFolders.clear();
		for (const auto & v: o["VideoFolders"].toArray())
			videoFolders << v.toString();
	}
	const QJsonObject bind = o["Bindings"].toObject();
	for (auto it = bind.begin(); it != bind.end(); ++it)
		bindings[it.key()] = it.value().toString();
}

void StreamerSettings::save()
{
	QJsonObject o;
	o["SelectedTab"] = selectedTab;
	QJsonObject acc;
	for (auto it = accounts.begin(); it != accounts.end(); ++it)
		if (!it.value().isEmpty())
			acc[QString::number(it.key())] = QJsonArray::fromStringList(it.value());
	o["Accounts"] = acc;
	o["PreferredCodec"] = int(preferredCodec);
	o["EnableAQ"] = enableAQ;
	o["Enable2Pass"] = enable2Pass;
	o["AdaptiveEncoderSpeed"] = adaptiveEncoderSpeed;
	o["NvencPreset"] = nvencPreset;
	o["AmdEncoderSpeed"] = amdEncoderSpeed;
	o["AutoUpdate"] = autoUpdate;
	o["DualGpu"] = dualGpu;
	o["GameGpu"] = gameGpu;
	o["StreamingGpu"] = streamingGpu;
	o["SingleGpu"] = singleGpu;
	o["GamepadEmulation"] = int(gamepadEmulation);
	o["AudioStreaming"] = int(audioStreaming);
	o["UseVirtualAudioDriver"] = useVirtualAudioDriver;
	o["EncryptLocalTraffic"] = encryptLocalTraffic;
	o["AutoAdjustBitrate"] = autoAdjustBitrate;
	o["StartWithLogin"] = startWithLogin;
	o["StartMinimizedInTray"] = startMinimizedInTray;
	o["UseTouchInput"] = useTouchInput;
	o["LockComputer"] = lockComputer;
	o["AutoSelectMicrophone"] = autoSelectMicrophone;
	o["ShowPairingRequests"] = showPairingRequests;
	o["CheckSetupAtStartup"] = checkSetupAtStartup;
	o["HorizontalFovTangent"] = horizontalFovTangent;
	o["VerticalFovTangent"] = verticalFovTangent;
	o["RenderResolutionOverride"] = renderResolutionOverride;
	o["FoveaSize"] = foveaSize;
	o["HandPose"] = handPose;
	o["PeripheryScale"] = peripheryScale;
	o["UseSharpening"] = useSharpening;
	o["Sharpening"] = sharpening;
	o["UseFovStencil"] = useFovStencil;
	o["SoftFoveaEdge"] = softFoveaEdge;
	o["BoostGamePriority"] = boostGamePriority;
	o["MouthSensitivity"] = mouthSensitivity;
	o["ReportedStreamerVersion"] = reportedStreamerVersion;
	o["ReportedVersionAuto"] = reportedVersionAuto;

	o["ScreenshotsRootPath"] = screenshotsRootPath;
	o["VideosRootPath"] = videosRootPath;
	o["VideoFolders"] = QJsonArray::fromStringList(videoFolders);
	o["StartupApps"] = QJsonArray::fromStringList(startupApps);
	QJsonObject bind;
	for (auto it = bindings.begin(); it != bindings.end(); ++it)
		bind[it.key()] = it.value();
	o["Bindings"] = bind;

	QDir().mkpath(QFileInfo(path()).path());
	QFile f(path());
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(QJsonDocument(o).toJson());
	emit changed();
}
