#include "face_tracking.h"
#include "platform.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <map>
#include <optional>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace od
{
namespace
{
const char * const shape_names[88] = {
	"EyeSquintRight",
	"EyeSquintLeft",
	"EyeWideRight",
	"EyeWideLeft",
	"BrowPinchRight",
	"BrowPinchLeft",
	"BrowLowererRight",
	"BrowLowererLeft",
	"BrowInnerUpRight",
	"BrowInnerUpLeft",
	"BrowOuterUpRight",
	"BrowOuterUpLeft",
	"NasalDilationRight",
	"NasalDilationLeft",
	"NasalConstrictRight",
	"NasalConstrictLeft",
	"CheekSquintRight",
	"CheekSquintLeft",
	"CheekPuffRight",
	"CheekPuffLeft",
	"CheekSuckRight",
	"CheekSuckLeft",
	"JawOpen",
	"JawRight",
	"JawLeft",
	"JawForward",
	"JawBackward",
	"JawClench",
	"JawMandibleRaise",
	"MouthClosed",
	"LipSuckUpperRight",
	"LipSuckUpperLeft",
	"LipSuckLowerRight",
	"LipSuckLowerLeft",
	"LipSuckCornerRight",
	"LipSuckCornerLeft",
	"LipFunnelUpperRight",
	"LipFunnelUpperLeft",
	"LipFunnelLowerRight",
	"LipFunnelLowerLeft",
	"LipPuckerUpperRight",
	"LipPuckerUpperLeft",
	"LipPuckerLowerRight",
	"LipPuckerLowerLeft",
	"MouthUpperUpRight",
	"MouthUpperUpLeft",
	"MouthUpperDeepenRight",
	"MouthUpperDeepenLeft",
	"NoseSneerRight",
	"NoseSneerLeft",
	"MouthLowerDownRight",
	"MouthLowerDownLeft",
	"MouthUpperRight",
	"MouthUpperLeft",
	"MouthLowerRight",
	"MouthLowerLeft",
	"MouthCornerPullRight",
	"MouthCornerPullLeft",
	"MouthCornerSlantRight",
	"MouthCornerSlantLeft",
	"MouthFrownRight",
	"MouthFrownLeft",
	"MouthStretchRight",
	"MouthStretchLeft",
	"MouthDimpleRight",
	"MouthDimpleLeft",
	"MouthRaiserUpper",
	"MouthRaiserLower",
	"MouthPressRight",
	"MouthPressLeft",
	"MouthTightenerRight",
	"MouthTightenerLeft",
	"TongueOut",
	"TongueUp",
	"TongueDown",
	"TongueRight",
	"TongueLeft",
	"TongueRoll",
	"TongueBendDown",
	"TongueCurlUp",
	"TongueSquish",
	"TongueFlat",
	"TongueTwistRight",
	"TongueTwistLeft",
	"SoftPalateClose",
	"ThroatSwallow",
	"NeckFlexRight",
	"NeckFlexLeft",
};

// FrameData face/eye block (Upstream.Mobile.FrameData, see runtime/openxr/linux_bodystate.cpp)
constexpr size_t eye_valid = 4632, eye_pose[2] = {4636, 4664};
constexpr size_t face_flags = 4700, face_eye_following = 4701, face_weights = 4704;
constexpr size_t face_end = face_weights + 70 * 4;

// OpenDesk's FB expression indices (Expressions.cs)
enum fb
{
	BrowLowererL, BrowLowererR, CheekPuffL, CheekPuffR, CheekRaiserL, CheekRaiserR, CheekSuckL, CheekSuckR, ChinRaiserB,
	ChinRaiserT, DimplerL, DimplerR, EyesClosedL, EyesClosedR, EyesLookDownL, EyesLookDownR, EyesLookLeftL, EyesLookLeftR,
	EyesLookRightL, EyesLookRightR, EyesLookUpL, EyesLookUpR, InnerBrowRaiserL, InnerBrowRaiserR, JawDrop, JawSidewaysLeft,
	JawSidewaysRight, JawThrust, LidTightenerL, LidTightenerR, LipCornerDepressorL, LipCornerDepressorR, LipCornerPullerL,
	LipCornerPullerR, LipFunnelerLb, LipFunnelerLt, LipFunnelerRb, LipFunnelerRt, LipPressorL, LipPressorR, LipPuckerL,
	LipPuckerR, LipStretcherL, LipStretcherR, LipSuckLb, LipSuckLt, LipSuckRb, LipSuckRt, LipTightenerL, LipTightenerR,
	LipsToward, LowerLipDepressorL, LowerLipDepressorR, MouthLeft, MouthRight, NoseWrinklerL, NoseWrinklerR,
	OuterBrowRaiserL, OuterBrowRaiserR, UpperLidRaiserL, UpperLidRaiserR, UpperLipRaiserL, UpperLipRaiserR,
	TongueTipInterdental, TongueTipAlveolar, TongueFrontDorsalPalate, TongueMidDorsalPalate, TongueBackDorsalVelar,
	TongueOut, TongueRetreat
};

float get_f(const uint8_t * d, size_t o)
{
	float v;
	std::memcpy(&v, d + o, 4);
	return v;
}

using data = face_tracking::shape_data;
using float_fn = std::function<float(const data &)>;

float simple(const data & e, int which) // UnifiedSimplifier
{
	const auto & s = e.shapes;
	switch (which)
	{
		case 0: return s[10] * 0.6f + s[8] * 0.4f;        // BrowUpRight
		case 1: return s[11] * 0.6f + s[9] * 0.4f;        // BrowUpLeft
		case 2: return s[6] * 0.75f + s[4] * 0.25f;       // BrowDownRight
		case 3: return s[7] * 0.75f + s[5] * 0.25f;       // BrowDownLeft
		case 4: return s[56] * 0.8f + s[58] * 0.2f;       // MouthSmileRight
		case 5: return s[57] * 0.8f + s[59] * 0.2f;       // MouthSmileLeft
		case 6: return s[60] > s[62] ? s[60] : s[62];     // MouthSadRight
		default: return s[61] > s[63] ? s[61] : s[63];    // MouthSadLeft
	}
}
const char * const simple_names[8] = {"BrowUpRight", "BrowUpLeft", "BrowDownRight", "BrowDownLeft", "MouthSmileRight",
                                      "MouthSmileLeft", "MouthSadRight", "MouthSadLeft"};
enum { BrowUpRight, BrowUpLeft, BrowDownRight, BrowDownLeft, MouthSmileRight, MouthSmileLeft, MouthSadRight, MouthSadLeft };

struct named
{
	std::string name;
	float_fn value;
	float bool_threshold;
};

// VRCFaceTracking's v2 parameters: every shape, every simplified shape and the combined shapes (UnifiedExpressionsParameters)
std::vector<named> v2_parameters()
{
	std::vector<named> out;
	for (int i = 0; i < 88; ++i)
		out.push_back({std::string("v2/") + shape_names[i], [i](const data & e) { return e.shapes[size_t(i)]; }, 0.f});
	for (int i = 0; i < 8; ++i)
		out.push_back({std::string("v2/") + simple_names[i], [i](const data & e) { return simple(e, i); }, 0.f});
	auto add = [&](const char * n, float_fn f) { out.push_back({n, std::move(f), 0.5f}); };
	auto xy = [&](const char * n, std::function<std::pair<float, float>(const data &)> f) {
		add((std::string(n) + "X").c_str(), [f](const data & e) { return f(e).first; });
		add((std::string(n) + "Y").c_str(), [f](const data & e) { return f(e).second; });
	};
#define S(i) e.shapes[i]
	xy("v2/Eye", [](const data & e) { return std::pair{(e.left.gaze_x + e.right.gaze_x) / 2, (e.left.gaze_y + e.right.gaze_y) / 2}; });
	xy("v2/EyeLeft", [](const data & e) { return std::pair{e.left.gaze_x, e.left.gaze_y}; });
	xy("v2/EyeRight", [](const data & e) { return std::pair{e.right.gaze_x, e.right.gaze_y}; });
	add("v2/PupilDilation", [](const data &) { return 0.5f; }); // headsets report no pupil size (upstream: fixed 5 mm, range 0..10)
	add("v2/PupilDiameterLeft", [](const data & e) { return e.left.pupil_mm * 0.1f; });
	add("v2/PupilDiameterRight", [](const data & e) { return e.right.pupil_mm * 0.1f; });
	add("v2/PupilDiameter", [](const data & e) { return (e.left.pupil_mm + e.right.pupil_mm) * 0.05f; });
	add("v2/EyeOpenLeft", [](const data & e) { return e.left.openness; });
	add("v2/EyeOpenRight", [](const data & e) { return e.right.openness; });
	add("v2/EyeOpen", [](const data & e) { return (e.left.openness + e.right.openness) / 2; });
	add("v2/EyeClosedLeft", [](const data & e) { return 1 - e.left.openness; });
	add("v2/EyeClosedRight", [](const data & e) { return 1 - e.right.openness; });
	add("v2/EyeClosed", [](const data & e) { return 1 - (e.left.openness + e.right.openness) / 2; });
	add("v2/EyeWide", [](const data & e) { return S(3) > S(2) ? S(3) : S(2); });
	add("v2/EyeLidLeft", [](const data & e) { return e.left.openness * 0.75f + S(3) * 0.25f; });
	add("v2/EyeLidRight", [](const data & e) { return e.right.openness * 0.75f + S(2) * 0.25f; });
	add("v2/EyeLid", [](const data & e) { return (e.left.openness + e.right.openness) / 2 * 0.75f + (S(2) + S(3)) / 2 * 0.25f; });
	add("v2/EyeSquint", [](const data & e) { return S(1) > S(0) ? S(1) : S(0); });
	add("v2/EyesSquint", [](const data & e) { return S(1) > S(0) ? S(1) : S(0); });
	add("v2/BrowUp", [](const data & e) { return (simple(e, BrowUpRight) + simple(e, BrowUpLeft)) * 0.5f; });
	add("v2/BrowDown", [](const data & e) { return (simple(e, BrowDownRight) + simple(e, BrowDownLeft)) * 0.5f; });
	add("v2/BrowInnerUp", [](const data & e) { return (S(9) + S(8)) / 2; });
	add("v2/BrowOuterUp", [](const data & e) { return (S(11) + S(10)) / 2; });
	add("v2/BrowExpressionRight", [](const data & e) { return std::min(1.f, S(8) * 0.5f + S(10) * 0.5f) - simple(e, BrowDownRight); });
	add("v2/BrowExpressionLeft", [](const data & e) { return std::min(1.f, S(9) * 0.5f + S(11) * 0.5f) - simple(e, BrowDownLeft); });
	add("v2/BrowExpression", [](const data & e) {
		return (std::min(1.f, (S(8) + S(10)) * 0.5f) - simple(e, BrowDownRight) + std::min(1.f, (S(9) + S(11)) * 0.5f) -
		        simple(e, BrowDownLeft)) * 0.5f;
	});
	add("v2/JawX", [](const data & e) { return S(23) - S(24); });
	add("v2/JawZ", [](const data & e) { return S(25) - S(26); });
	add("v2/CheekSquint", [](const data & e) { return (S(17) + S(16)) / 2; });
	add("v2/CheekPuffSuckLeft", [](const data & e) { return S(19) - S(21); });
	add("v2/CheekPuffSuckRight", [](const data & e) { return S(18) - S(20); });
	add("v2/CheekPuffSuck", [](const data & e) { return (S(18) + S(19)) / 2 - (S(20) + S(21)) / 2; });
	add("v2/CheekSuck", [](const data & e) { return (S(21) + S(20)) / 2; });
	add("v2/MouthUpperX", [](const data & e) { return S(52) - S(53); });
	add("v2/MouthLowerX", [](const data & e) { return S(54) - S(55); });
	add("v2/MouthX", [](const data & e) { return (S(52) + S(54)) / 2 - (S(53) + S(55)) / 2; });
	add("v2/LipSuckUpper", [](const data & e) { return (S(30) + S(31)) / 2; });
	add("v2/LipSuckLower", [](const data & e) { return (S(32) + S(33)) / 2; });
	add("v2/LipSuck", [](const data & e) { return (S(30) + S(31) + S(32) + S(33)) / 4; });
	add("v2/LipFunnelUpper", [](const data & e) { return (S(36) + S(37)) / 2; });
	add("v2/LipFunnelLower", [](const data & e) { return (S(38) + S(39)) / 2; });
	add("v2/LipFunnel", [](const data & e) { return (S(36) + S(37) + S(38) + S(39)) / 4; });
	add("v2/LipPuckerUpper", [](const data & e) { return (S(40) + S(41)) / 2; });
	add("v2/LipPuckerLower", [](const data & e) { return (S(42) + S(43)) / 2; });
	add("v2/LipPuckerRight", [](const data & e) { return (S(40) + S(42)) / 2; });
	add("v2/LipPuckerLeft", [](const data & e) { return (S(41) + S(43)) / 2; });
	add("v2/LipPucker", [](const data & e) { return (S(40) + S(41) + S(42) + S(43)) / 4; });
	add("v2/LipSuckFunnelUpper", [](const data & e) { return (S(30) + S(31)) / 2 - (S(36) + S(37)) / 2; });
	add("v2/LipSuckFunnelLower", [](const data & e) { return (S(32) + S(33)) / 2 - (S(38) + S(39)) / 2; });
	add("v2/LipSuckFunnelLowerLeft", [](const data & e) { return S(33) - S(39); });
	add("v2/LipSuckFunnelLowerRight", [](const data & e) { return S(32) - S(38); });
	add("v2/LipSuckFunnelUpperLeft", [](const data & e) { return S(31) - S(37); });
	add("v2/LipSuckFunnelUpperRight", [](const data & e) { return S(30) - S(36); });
	add("v2/MouthUpperUp", [](const data & e) { return S(44) * 0.5f + S(45) * 0.5f; });
	add("v2/MouthLowerDown", [](const data & e) { return S(50) * 0.5f + S(51) * 0.5f; });
	add("v2/MouthOpen", [](const data & e) { return S(44) * 0.25f + S(45) * 0.25f + S(50) * 0.25f + S(51) * 0.25f; });
	add("v2/MouthStretch", [](const data & e) { return (S(62) + S(63)) / 2; });
	add("v2/MouthTightener", [](const data & e) { return (S(70) + S(71)) / 2; });
	add("v2/MouthPress", [](const data & e) { return (S(68) + S(69)) / 2; });
	add("v2/MouthDimple", [](const data & e) { return (S(64) + S(65)) / 2; });
	add("v2/NoseSneer", [](const data & e) { return (S(48) + S(49)) / 2; });
	add("v2/MouthTightenerStretch", [](const data & e) { return (S(70) + S(71)) / 2 - (S(62) + S(63)) / 2; });
	add("v2/MouthTightenerStretchLeft", [](const data & e) { return S(71) - S(63); });
	add("v2/MouthTightenerStretchRight", [](const data & e) { return S(70) - S(62); });
	add("v2/MouthCornerYLeft", [](const data & e) { return S(59) - S(61); });
	add("v2/MouthCornerYRight", [](const data & e) { return S(58) - S(60); });
	add("v2/MouthCornerY", [](const data & e) { return (S(59) - S(61) + S(58) - S(60)) * 0.5f; });
	add("v2/SmileFrownRight", [](const data & e) { return simple(e, MouthSmileRight) - S(60); });
	add("v2/SmileFrownLeft", [](const data & e) { return simple(e, MouthSmileLeft) - S(61); });
	add("v2/SmileFrown", [](const data & e) {
		return simple(e, MouthSmileRight) * 0.5f + simple(e, MouthSmileLeft) * 0.5f - S(60) * 0.5f - S(61) * 0.5f;
	});
	add("v2/SmileSadRight", [](const data & e) { return simple(e, MouthSmileRight) - simple(e, MouthSadRight); });
	add("v2/SmileSadLeft", [](const data & e) { return simple(e, MouthSmileLeft) - simple(e, MouthSadLeft); });
	add("v2/SmileSad", [](const data & e) {
		return (simple(e, MouthSmileLeft) + simple(e, MouthSmileRight)) / 2 - (simple(e, MouthSadLeft) + simple(e, MouthSadRight)) / 2;
	});
	add("v2/TongueX", [](const data & e) { return S(75) - S(76); });
	add("v2/TongueY", [](const data & e) { return S(73) - S(74); });
	add("v2/TongueArchY", [](const data & e) { return S(79) - S(78); });
	add("v2/TongueShape", [](const data & e) { return S(81) - S(80); });
#undef S
	return out;
}

enum class ptype { f, b, i };

struct avatar_param
{
	std::string name, address;
	ptype type;
};

// "(?<!(v\d+))(/NAME)$|^(NAME)$" without lookbehind: does `address` end in "/name" (not right after "v<digits>"),
// or is it exactly name? with_digits: "/NAME<digits>" (binary parameters)
bool address_matches(const std::string & address, const std::string & name, bool with_digits)
{
	std::string a = address;
	if (with_digits)
	{
		size_t end = a.size();
		while (end > 0 && std::isdigit(static_cast<unsigned char>(a[end - 1])))
			--end;
		if (end == a.size())
			return false;
		a.resize(end);
	}
	if (a == name)
		return true;
	const std::string suffix = "/" + name;
	if (a.size() < suffix.size() || a.compare(a.size() - suffix.size(), suffix.size(), suffix) != 0)
		return false;
	std::string before = a.substr(0, a.size() - suffix.size()); // must not end in v<digits>
	size_t d = before.size();
	while (d > 0 && std::isdigit(static_cast<unsigned char>(before[d - 1])))
		--d;
	return !(d < before.size() && d > 0 && before[d - 1] == 'v');
}

// ---- OSC
void pad(std::string & b)
{
	do
		b.push_back('\0');
	while (b.size() % 4);
}
void put_be32(std::string & b, uint32_t v)
{
	v = htonl(v);
	b.append(reinterpret_cast<const char *>(&v), 4);
}
std::string osc_message(const std::string & address, const std::string & tags, const std::vector<float> & floats)
{
	std::string b = address;
	pad(b);
	b += "," + tags;
	pad(b);
	for (float f: floats)
	{
		uint32_t u;
		std::memcpy(&u, &f, 4);
		put_be32(b, u);
	}
	return b;
}
} // namespace

struct face_tracking::impl
{
	std::function<void(const std::string &)> log;
#ifdef _WIN32
	using socket_t = SOCKET;
	static constexpr socket_t no_socket = INVALID_SOCKET;
#else
	using socket_t = int;
	static constexpr socket_t no_socket = -1;
#endif
	socket_t sock = no_socket, recv_sock = no_socket;
	sockaddr_in target{};
	std::vector<named> parameters = v2_parameters();
	float mouth_sensitivity = 1;

	// what the current avatar takes, resolved per parameter
	struct sink
	{
		const named * source;
		std::string float_address, bool_address;
		std::vector<std::pair<std::string, int>> binary; // address, bit
		int binary_max = 0;
		std::string negative_address;
		std::optional<float> last_float;
		std::optional<bool> last_bool, last_negative;
		std::vector<std::optional<bool>> last_bits;
	};
	std::vector<sink> sinks;
	bool native_gaze = true, native_closed = true; // VRChat native eye tracking when the avatar has no eye parameters
	std::string avatar_file;
	int64_t avatar_mtime = 0;
	std::string pending_avatar_id;
	bool had_data = false;
	std::chrono::steady_clock::time_point next_eye_log{};
	bool vrchat = false; // VRChat.exe runs (Proton): only then anything is sent

	static int64_t now_ns()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	std::map<std::string, std::optional<bool>> active_last;
	// VRCFaceTracking's status flags avatars switch their face tracking layers with, at the avatar's own addresses
	// (often prefixed, e.g. /avatar/parameters/FT/EyeTrackingActive)
	std::map<std::string, std::vector<std::string>> status_addresses;
	int64_t next_full_send_ns = 0;

	void send(const std::string & packet)
	{
		if (sock == no_socket)
			return;
#ifdef _WIN32
		sendto(sock, packet.data(), int(packet.size()), 0, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
#else
		sendto(sock, packet.data(), packet.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&target), sizeof(target));
#endif
	}

	static std::vector<QString> osc_roots()
	{
		if (const char * dir = std::getenv("OPENDESK_VRCHAT_OSC_DIR")) // tests
			return {QString::fromUtf8(dir)};
		// VRChat under Proton: <library>/steamapps/compatdata/438100/pfx/.../LocalLow/VRChat/VRChat/OSC
		std::vector<QString> libraries{QDir::homePath() + "/.local/share/Steam", QDir::homePath() + "/.steam/steam"};
		QFile vdf(QDir::homePath() + "/.local/share/Steam/steamapps/libraryfolders.vdf");
		if (vdf.open(QIODevice::ReadOnly))
			for (const QString & line: QString::fromUtf8(vdf.readAll()).split('\n'))
				if (line.trimmed().startsWith("\"path\""))
					libraries.push_back(line.section('"', 3, 3));
		std::vector<QString> roots;
		for (const QString & l: libraries)
		{
			QString r = l + "/steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/LocalLow/VRChat/VRChat/OSC";
			QString canonical = QFileInfo(r).canonicalFilePath();
			if (!canonical.isEmpty() && std::find(roots.begin(), roots.end(), canonical) == roots.end())
				roots.push_back(canonical);
		}
		return roots;
	}

	// the avatar config VRChat wrote: by id (avatar change) or the most recent one (VRChat writes it on load)
	QString find_avatar(const std::string & id)
	{
		QString best;
		QDateTime newest;
		for (const QString & root: osc_roots())
		{
			QDirIterator it(root, {"*.json"}, QDir::Files, QDirIterator::Subdirectories);
			while (it.hasNext())
			{
				QString f = it.next();
				if (!f.contains("/Avatars/"))
					continue;
				if (!id.empty())
				{
					if (QFileInfo(f).completeBaseName() == QString::fromStdString(id))
						return f;
					continue;
				}
				QDateTime m = QFileInfo(f).lastModified();
				if (best.isEmpty() || m > newest)
				{
					best = f;
					newest = m;
				}
			}
		}
		return best;
	}

	void load_avatar(const QString & file)
	{
		QFile f(file);
		if (!f.open(QIODevice::ReadOnly))
			return;
		// VRChat writes these with a UTF-8 BOM
		QByteArray raw = f.readAll();
		if (raw.startsWith("\xEF\xBB\xBF"))
			raw.remove(0, 3);
		QJsonObject o = QJsonDocument::fromJson(raw).object();
		std::vector<avatar_param> params;
		for (const auto & v: o.value("parameters").toArray())
		{
			QJsonObject p = v.toObject();
			QJsonObject in = p.value("input").toObject();
			if (in.isEmpty())
				continue; // read-only (output) parameters
			QString t = in.value("type").toString();
			params.push_back({p.value("name").toString().toStdString(), in.value("address").toString().toStdString(),
			                  t == "Bool" ? ptype::b : t == "Int" ? ptype::i : ptype::f});
		}
		resolve(params, o.value("fullFaceTracking").toBool());
		avatar_file = file.toStdString();
		avatar_mtime = QFileInfo(file).lastModified().toMSecsSinceEpoch();
		log(std::format("[face] avatar {} ({}): {} face tracking parameters{}", o.value("name").toString().toStdString(),
		                o.value("id").toString().toStdString(), sinks.size(),
		                native_gaze || native_closed ? ", VRChat native eye tracking" : ""));
	}

	void resolve(const std::vector<avatar_param> & params, bool full_face_tracking)
	{
		sinks.clear();
		bool has_eye_xy = false, has_eye_lid = false;
		if (!full_face_tracking)
			for (const auto & n: parameters)
			{
				sink s{&n};
				for (const auto & a: params)
				{
					if (a.type == ptype::f && s.float_address.empty() && address_matches(a.address, n.name, false))
						s.float_address = a.address;
					if (a.type == ptype::b && s.bool_address.empty() && address_matches(a.address, n.name, false))
						s.bool_address = a.address;
					if (a.type == ptype::b && s.negative_address.empty() && address_matches(a.address, n.name + "Negative", false))
						s.negative_address = a.address;
					if (a.type == ptype::b && address_matches(a.address, n.name, true))
					{
						// "Name<1|2|4|8...>": bit log2(number)
						size_t d = a.address.size();
						while (d > 0 && std::isdigit(static_cast<unsigned char>(a.address[d - 1])))
							--d;
						int v = std::atoi(a.address.c_str() + d);
						if (v > 0 && (v & (v - 1)) == 0)
							s.binary.push_back({a.address, int(std::log2(v))});
					}
				}
				if (!s.binary.empty())
					s.binary_max = 1 << s.binary.size();
				s.last_bits.resize(s.binary.size());
				if (s.float_address.empty() && s.bool_address.empty() && s.binary.empty())
					continue;
				const std::string & nm = n.name;
				if (nm.find("Eye") != std::string::npos && (nm.back() == 'X' || nm.back() == 'Y'))
					has_eye_xy = true;
				if (nm.find("Eye") != std::string::npos && (nm.find("Open") != std::string::npos || nm.find("Lid") != std::string::npos))
					has_eye_lid = true;
				sinks.push_back(std::move(s));
			}
		native_gaze = !full_face_tracking && !has_eye_xy;
		native_closed = !full_face_tracking && !has_eye_lid;
		active_last.clear();
		status_addresses.clear();
		for (const char * name: {"EyeTrackingActive", "ExpressionTrackingActive", "LipTrackingActive"})
		{
			auto & list = status_addresses[name];
			for (const auto & a: params)
				if (a.type == ptype::b && address_matches(a.address, name, false))
					list.push_back(a.address);
			if (list.empty())
				list.push_back(std::string("/avatar/parameters/") + name);
		}
		next_full_send_ns = 0;
	}

	void publish(const data & d)
	{
		// everything again once a second: a value VRChat missed (not listening yet, avatar still loading) would
		// otherwise stay wrong until it changes - fatal for the status flags that switch an avatar's face tracking on
		if (const int64_t now = now_ns(); now >= next_full_send_ns)
		{
			next_full_send_ns = now + 1000000000;
			for (auto & s: sinks)
			{
				s.last_float.reset();
				s.last_bool.reset();
				s.last_negative.reset();
				for (auto & b: s.last_bits)
					b.reset();
			}
			active_last.clear();
		}
		std::string bundle_items;
		auto add = [&](std::string msg) {
			if (bundle_items.size() + msg.size() > 1300)
				flush(bundle_items);
			put_be32(bundle_items, uint32_t(msg.size()));
			bundle_items += msg;
		};
		for (auto & s: sinks)
		{
			float v = s.source->value(d);
			if (!std::isfinite(v))
				v = 0;
			if (!s.float_address.empty() && s.last_float != v)
			{
				s.last_float = v;
				add(osc_message(s.float_address, "f", {v}));
			}
			if (!s.bool_address.empty())
			{
				bool b = v < s.source->bool_threshold;
				if (s.last_bool != b)
				{
					s.last_bool = b;
					add(osc_message(s.bool_address, b ? "T" : "F", {}));
				}
			}
			if (!s.binary.empty())
			{
				bool negative = v < 0;
				if (!s.negative_address.empty() && s.last_negative != negative)
				{
					s.last_negative = negative;
					add(osc_message(s.negative_address, negative ? "T" : "F", {}));
				}
				float a = std::abs(v);
				for (size_t i = 0; i < s.binary.size(); ++i)
				{
					bool bit = s.negative_address.empty() && negative ? false
					           : a > 0.99999f                         ? true
					                                                  : ((int(a * float(s.binary_max)) >> s.binary[i].second) & 1) == 1;
					if (s.last_bits[i] != bit)
					{
						s.last_bits[i] = bit;
						add(osc_message(s.binary[i].first, bit ? "T" : "F", {}));
					}
				}
			}
		}
		// VRCFaceTracking's status parameters (any prefix)
		auto status = [&](const char * name, bool on) {
			auto & last = active_last[name];
			if (last == on)
				return;
			last = on;
			auto it = status_addresses.find(name);
			if (it == status_addresses.end())
				add(osc_message(std::string("/avatar/parameters/") + name, on ? "T" : "F", {}));
			else
				for (const auto & address: it->second)
					add(osc_message(address, on ? "T" : "F", {}));
		};
		status("EyeTrackingActive", d.eye_active);
		status("ExpressionTrackingActive", d.expression_active);
		status("LipTrackingActive", d.expression_active);
		if (d.eye_active && native_gaze) // degrees: pitch, yaw per eye
			add(osc_message("/tracking/eye/LeftRightPitchYaw", "ffff",
			                {-float(std::atan(d.left.gaze_y) * 180 / M_PI), float(std::atan(d.left.gaze_x) * 180 / M_PI),
			                 -float(std::atan(d.right.gaze_y) * 180 / M_PI), float(std::atan(d.right.gaze_x) * 180 / M_PI)}));
		if (d.eye_active && native_closed)
			add(osc_message("/tracking/eye/EyesClosedAmount", "f", {1 - (d.left.openness + d.right.openness) / 2}));
		flush(bundle_items);
	}

	void flush(std::string & items)
	{
		if (items.empty())
			return;
		std::string b = "#bundle";
		b.push_back('\0');
		put_be32(b, 0);
		put_be32(b, 1); // timetag: immediately
		b += items;
		send(b);
		items.clear();
	}
};

face_tracking::face_tracking(std::function<void(const std::string &)> log, std::string host, int port) :
        p(std::make_unique<impl>())
{
	p->log = std::move(log);
#ifdef _WIN32
	WSADATA wsa{};
	WSAStartup(MAKEWORD(2, 2), &wsa);
	p->sock = socket(AF_INET, SOCK_DGRAM, 0);
#else
	p->sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
#endif
	p->target.sin_family = AF_INET;
	if (const char * test_port = std::getenv("OPENDESK_FACE_OSC_PORT")) // tests
		port = std::atoi(test_port);
	p->target.sin_port = htons(uint16_t(port));
	inet_pton(AF_INET, host.c_str(), &p->target.sin_addr);
	// VRChat announces avatar changes on udp 9001 (VRCFaceTracking listens there too: then we see the files only)
#ifdef _WIN32
	p->recv_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (p->recv_sock != impl::no_socket)
	{
		u_long nonblocking = 1;
		ioctlsocket(p->recv_sock, FIONBIO, &nonblocking);
	}
#else
	p->recv_sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#endif
	sockaddr_in local{};
	local.sin_family = AF_INET;
	local.sin_port = htons(9001);
	local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (p->recv_sock == impl::no_socket || bind(p->recv_sock, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0)
	{
#ifdef _WIN32
		if (p->recv_sock != impl::no_socket)
			closesocket(p->recv_sock);
#else
		close(p->recv_sock);
#endif
		p->recv_sock = impl::no_socket;
		p->log("[face] udp 9001 is taken (VRCFaceTracking running?): following VRChat's avatar config files only");
	}
	tick();
}

face_tracking::~face_tracking()
{
#ifdef _WIN32
	if (p->sock != impl::no_socket)
		closesocket(p->sock);
	if (p->recv_sock != impl::no_socket)
		closesocket(p->recv_sock);
	WSACleanup();
#else
	if (p->sock != impl::no_socket)
		close(p->sock);
	if (p->recv_sock != impl::no_socket)
		close(p->recv_sock);
#endif
}

bool face_tracking::unify(const uint8_t * d, size_t size, shape_data & out)
{
	if (size < face_end)
		return false;
	const bool eyes[2] = {d[eye_valid] != 0, d[eye_valid + 1] != 0};
	const uint8_t flags = d[face_flags];
	const bool following = d[face_eye_following] != 0;
	float w[70];
	std::memcpy(w, d + face_weights, sizeof(w));
	auto & s = out.shapes;
	out.eye_active = eyes[0] || eyes[1] || following;
	out.expression_active = (flags & 1) != 0;
	if (eyes[0] || eyes[1])
		for (int e = 0; e < 2; ++e)
		{
			// OpenDesk's Quaternion.Cartesian(): (asin(2(xz - wy)), atan2(2(yz + wx), w2 - x2 - y2 + z2))
			float q[4];
			std::memcpy(q, d + eye_pose[e], 16);
			float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			if (n <= 0)
				continue;
			float x = q[0] / n, y = q[1] / n, z = q[2] / n, ww = q[3] / n;
			auto & eye = e == 0 ? out.left : out.right;
			eye.gaze_x = std::asin(std::clamp(2 * (x * z - ww * y), -1.f, 1.f));
			eye.gaze_y = std::atan2(2 * (y * z + ww * x), ww * ww - x * x - y * y + z * z);
			eye.pupil_mm = 5;
		}
	if (following) // eye openness and the eye/brow region
	{
		out.left.openness = 1 - std::clamp(w[EyesClosedL] + w[EyesClosedL] * w[LidTightenerL], 0.f, 1.f);
		out.right.openness = 1 - std::clamp(w[EyesClosedR] + w[EyesClosedR] * w[LidTightenerR], 0.f, 1.f);
		s[3] = w[UpperLidRaiserL];
		s[2] = w[UpperLidRaiserR];
		s[1] = w[LidTightenerL];
		s[0] = w[LidTightenerR];
		s[9] = w[InnerBrowRaiserL];
		s[8] = w[InnerBrowRaiserR];
		s[11] = w[OuterBrowRaiserL];
		s[10] = w[OuterBrowRaiserR];
		s[5] = s[7] = w[BrowLowererL];
		s[4] = s[6] = w[BrowLowererR];
	}
	if (flags & 1) // mouth, cheeks, tongue
	{
		s[22] = w[JawDrop];
		s[24] = w[JawSidewaysLeft];
		s[23] = w[JawSidewaysRight];
		s[25] = w[JawThrust];
		s[29] = w[LipsToward];
		s[53] = s[55] = w[MouthLeft];
		s[52] = s[54] = w[MouthRight];
		s[57] = s[59] = w[LipCornerPullerL];
		s[56] = s[58] = w[LipCornerPullerR];
		s[61] = w[LipCornerDepressorL];
		s[60] = w[LipCornerDepressorR];
		s[51] = w[LowerLipDepressorL];
		s[50] = w[LowerLipDepressorR];
		s[45] = s[47] = std::max(0.f, w[UpperLipRaiserL] - w[NoseWrinklerL]);
		s[44] = s[46] = std::max(0.f, w[UpperLipRaiserR] - w[NoseWrinklerR]);
		s[66] = w[ChinRaiserT];
		s[67] = w[ChinRaiserB];
		s[65] = w[DimplerL];
		s[64] = w[DimplerR];
		s[71] = w[LipTightenerL];
		s[70] = w[LipTightenerR];
		s[69] = w[LipPressorL];
		s[68] = w[LipPressorR];
		s[63] = w[LipStretcherL];
		s[62] = w[LipStretcherR];
		s[40] = s[42] = w[LipPuckerR];
		s[41] = s[43] = w[LipPuckerL];
		s[37] = w[LipFunnelerLt];
		s[36] = w[LipFunnelerRt];
		s[39] = w[LipFunnelerLb];
		s[38] = w[LipFunnelerRb];
		s[31] = std::min(1 - std::pow(w[UpperLipRaiserL], 1.f / 6), w[LipSuckLt]);
		s[30] = std::min(1 - std::pow(w[UpperLipRaiserR], 1.f / 6), w[LipSuckRt]);
		s[33] = w[LipSuckLb];
		s[32] = w[LipSuckRb];
		s[19] = w[CheekPuffL];
		s[18] = w[CheekPuffR];
		s[21] = w[CheekSuckL];
		s[20] = w[CheekSuckR];
		s[17] = w[CheekRaiserL];
		s[16] = w[CheekRaiserR];
		s[49] = w[NoseWrinklerL];
		s[48] = w[NoseWrinklerR];
		if (flags & 2) // tongue
		{
			s[72] = w[TongueTipInterdental];
			s[76] = w[TongueTipAlveolar];
			s[75] = w[TongueFrontDorsalPalate];
			s[73] = w[TongueMidDorsalPalate];
			s[74] = w[TongueBackDorsalVelar];
		}
		else
		{
			s[72] = w[TongueOut];
			s[79] = w[TongueTipAlveolar];
		}
	}
	return out.eye_active || out.expression_active;
}

void face_tracking::set_mouth_sensitivity(float k)
{
	p->mouth_sensitivity = std::clamp(k, 1.f, 4.f);
}

void face_tracking::on_frame_data(const uint8_t * d, size_t size)
{
	if (!p->vrchat)
		return;
	shape_data data;
	bool tracked = unify(d, size, data);
	if (tracked && data.expression_active && p->mouth_sensitivity != 1)
		for (size_t i = 22; i <= 71; ++i) // jaw, lips, mouth (unified expressions 22-71)
			data.shapes[i] = 1 - std::pow(1 - std::clamp(data.shapes[i], 0.f, 1.f), p->mouth_sensitivity);
	if (!tracked)
	{
		if (p->had_data) // tracking stopped: say so once
		{
			p->had_data = false;
			p->publish(data);
		}
		return;
	}
	if (!p->had_data)
		p->log(std::format("[face] tracking data from the headset -> VRChat (OSC): eyes {}, face {}, tongue {}",
		                   d[eye_valid] || d[eye_valid + 1] ? "yes" : "no", (d[face_flags] & 1) ? "yes" : "no",
		                   (d[face_flags] & 2) ? "yes" : "no"));
	p->had_data = true;
	if (data.eye_active && std::chrono::steady_clock::now() >= p->next_eye_log)
	{
		// per eye as VRChat gets it: identical values mean the headset sends one gaze for both eyes
		p->next_eye_log = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		auto deg = [](float a) { return a * 180 / float(M_PI); };
		p->log(std::format("[face] eyes: left yaw {:.1f} pitch {:.1f}, right yaw {:.1f} pitch {:.1f} (degrees)", deg(data.left.gaze_x),
		                   deg(data.left.gaze_y), deg(data.right.gaze_x), deg(data.right.gaze_y)));
	}
	p->publish(data);
}

namespace
{
// Linux: VRChat under Proton, a process whose command line names VRChat.exe. Windows: VRChat.exe itself.
bool vrchat_process()
{
	return platform::any_process_has("VRChat.exe");
}
} // namespace

void face_tracking::tick()
{
	if (bool running = vrchat_process(); running != p->vrchat)
	{
		p->vrchat = running;
		p->log(running ? "[face] VRChat is running: face/eye tracking -> OSC" : "[face] VRChat closed: face/eye tracking paused");
		if (running) // resend everything to the new VRChat instance
		{
			p->avatar_file.clear();
			p->had_data = false;
		}
	}
	if (!p->vrchat)
		return;
	// /avatar/change "avtr_..." from VRChat
	if (p->recv_sock != impl::no_socket)
		for (;;)
		{
			char buf[2048];
#ifdef _WIN32
			int n = recv(p->recv_sock, buf, int(sizeof(buf)), 0);
#else
			ssize_t n = recv(p->recv_sock, buf, sizeof(buf), 0);
#endif
			if (n <= 0)
				break;
			std::string msg(buf, size_t(n));
			if (msg.rfind("/avatar/change", 0) == 0)
			{
				size_t tags = msg.find(",s");
				if (tags != std::string::npos)
				{
					size_t value = (tags + 2 + 4) & ~size_t(3);
					if (value < msg.size())
						p->pending_avatar_id = msg.substr(value, msg.find('\0', value) - value);
				}
			}
		}
	QString file = !p->pending_avatar_id.empty() ? p->find_avatar(p->pending_avatar_id) : p->find_avatar({});
	if (!file.isEmpty())
	{
		int64_t mtime = QFileInfo(file).lastModified().toMSecsSinceEpoch();
		if (file.toStdString() != p->avatar_file || mtime != p->avatar_mtime)
			p->load_avatar(file);
		p->pending_avatar_id.clear();
	}
}
} // namespace od
