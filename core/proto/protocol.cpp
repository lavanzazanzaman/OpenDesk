#include "protocol.h"

#include <cstdio>
#include <format>

namespace od
{
const aes_key broadcast_key{32, 71, 236, 94, 194, 34, 85, 255, 165, 172, 187, 150, 6, 104, 106, 57,
                            57, 62, 244, 114, 75, 174, 237, 9, 48, 36, 239, 82, 57, 98, 205, 80};
const aes_iv broadcast_iv{82, 200, 129, 118, 144, 104, 249, 4, 62, 20, 120, 110, 20, 180, 63, 31};

const char * message_name(uint8_t type)
{
	// all of Upstream.Net.MessageType (upstream 1.34.22, unchanged in 1.34.23)
	switch (type)
	{
		case 1: return "PeerInfo";
		case 3: return "CrashCallstack";
		case 4: return "UpdateRequired";
		case 5: return "UpdateUnavailable";
		case 6: return "UpdateInProgress";
		case 11: return "UserSettingsReload";
		case 12: return "UserSettingsPropertyChanged";
		case 13: return "MobileSettingsReload";
		case 14: return "MobileSettingsPropertyChanged";
		case 15: return "StreamerSettingsReload";
		case 16: return "StreamerSettingsPropertyChanged";
		case 21: return "BoundaryDataChanged";
		case 22: return "SkeletonDataChanged";
		case 23: return "RestartStream";
		case 24: return "ForceKeyFrame";
		case 25: return "SwitchMonitor";
		case 26: return "ToggleDictation";
		case 27: return "LaunchStreamVR";
		case 28: return "ExitSteamVR";
		case 29: return "Recenter";
		case 30: return "RequestGames";
		case 31: return "Games";
		case 32: return "RequestGameThumbnails";
		case 33: return "GameThumbnail";
		case 34: return "LaunchGame";
		case 35: return "ExitGame";
		case 36: return "GameExited";
		case 40: return "ShowDesktop";
		case 41: return "DisposeGamepadEmulation";
		case 42: return "AddMonitor";
		case 43: return "RemoveMonitor";
		case 44: return "SetMonitorPortrait";
		case 50: return "RequestVideos";
		case 51: return "Videos";
		case 52: return "RequestVideoThumbnails";
		case 53: return "VideoThumbnail";
		case 54: return "PlayVideo";
		case 55: return "DownloadVideo";
		case 56: return "StopVideo";
		case 57: return "VideoStopped";
		case 58: return "SeekVideo";
		case 59: return "CancelDownloadVideo";
		case 60: return "AudioTracks";
		case 61: return "SetAudioStreamIndex";
		case 62: return "PasteUrl";
		case 63: return "CancelPasteUrl";
		case 64: return "VideoAdded";
		case 65: return "SubtitleTracks";
		case 66: return "SetSubtitleTrackId";
		case 67: return "SetSubtitleStreamIndex";
		case 68: return "SetSubtitleOffsetAmount";
		case 70: return "ScreenshotToDesktop";
		case 71: return "Exit";
		case 72: return "EnableVRPassthrough";
		case 73: return "DisableVRPassthrough";
		case 74: return "ToggleVRPassthrough";
		case 75: return "ToggleHandPassthrough";
		case 76: return "ToggleDeskPassthrough";
		case 77: return "TogglePerformanceOverlay";
		case 79: return "ToggleFoveatedStreaming";
		case 80: return "ChangeLanguage";
		case 81: return "Disconnect";
		case 82: return "RequestCursor";
		case 83: return "Cursor";
		case 90: return "WakeUpComputer";
		case 91: return "ComputerAwaken";
		case 100: return "Log";
		case 101: return "ClipboardChanged";
		case 102: return "ClipboardRequestText";
		case 103: return "ClipboardRequestImage";
		case 104: return "ClipboardRequestFileDescriptors";
		case 105: return "ClipboardRequestFiles";
		case 106: return "ClipboardText";
		case 107: return "ClipboardImage";
		case 108: return "ClipboardFileDescriptors";
		case 109: return "ClipboardFiles";
		case 110: return "ClipboardCopying";
		case 111: return "ClipboardCopyingComplete";
		case 112: return "ClipboardLimitExceeded";
		case 113: return "ClipboardCopied";
		default: return nullptr;
	}
}

bytes encode_net_message(message_type t, const uint8_t * payload, size_t size)
{
	bytes out;
	out.reserve(size + 5);
	out.push_back(uint8_t(t));
	append_be32(out, int32_t(size));
	append(out, payload, size);
	return out;
}

std::optional<net_message> pop_net_message(bytes & plaintext)
{
	if (plaintext.size() < 5)
		return std::nullopt;
	int32_t len = read_be32(plaintext.data() + 1);
	if (len < 0 || plaintext.size() < size_t(len) + 5)
		return std::nullopt;
	net_message m{plaintext[0], bytes(plaintext.begin() + 5, plaintext.begin() + 5 + len)};
	plaintext.erase(plaintext.begin(), plaintext.begin() + 5 + len);
	return m;
}

static std::string xml_escape(std::string_view s)
{
	std::string out;
	for (char c: s)
	{
		switch (c)
		{
			case '&': out += "&amp;"; break;
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			default: out += c;
		}
	}
	return out;
}

std::string version_string(const std::array<int, 4> & v)
{
	return std::format("{}.{}.{}", v[0], v[1], v[2]);
}

std::array<int, 4> parse_streamer_version(std::string_view text)
{
	std::array<int, 4> v = default_streamer_version;
	// accepts "1", "1.34", "1.34.23", "1.34.23.0" and ignores a trailing suffix
	std::sscanf(std::string(text).c_str(), "%d.%d.%d.%d", &v[0], &v[1], &v[2], &v[3]);
	if (v[0] <= 0)
		return default_streamer_version;
	return v;
}

std::string computer_info::to_xml() const
{
	auto b = [](bool v) { return v ? "true" : "false"; };
	auto b64 = [](const auto & a) { return base64_encode(a.data(), a.size()); };
	std::string adapters_xml;
	for (const auto & a: adapters)
		adapters_xml += std::format("<NetworkAdapter><IPAddresString>{}</IPAddresString><IsGigabit>{}</IsGigabit>"
		                            "<IsUsb>{}</IsUsb><IsWireless>{}</IsWireless><MACAddresString>{}</MACAddresString>"
		                            "</NetworkAdapter>",
		                            xml_escape(a.ip), b(a.is_gigabit), b(a.is_usb), b(a.is_wireless), xml_escape(a.mac));
	return std::format(
	        "<Computer xmlns=\"http://schemas.datacontract.org/2004/07/VirtualDesktop.Interfaces\" "
	        "xmlns:i=\"http://www.w3.org/2001/XMLSchema-instance\">"
	        "<AllowRemoteConnections>{}</AllowRemoteConnections><ConnectionID>{}</ConnectionID>"
	        "<Description>{}</Description><EncryptLocalTraffic>{}</EncryptLocalTraffic>"
	        "<EncryptRemoteTraffic>{}</EncryptRemoteTraffic><ID>{}</ID><IV>{}</IV><Key>{}</Key><Name>{}</Name>"
	        "<OS>{}</OS><PrivateAdapters>{}</PrivateAdapters><Region>{}</Region>"
	        "<StreamerVersion xmlns:a=\"http://schemas.datacontract.org/2004/07/System\">"
	        "<a:_Build>{}</a:_Build><a:_Major>{}</a:_Major><a:_Minor>{}</a:_Minor><a:_Revision>{}</a:_Revision>"
	        "</StreamerVersion></Computer>",
	        b(allow_remote), b64(connection_id), xml_escape(description), b(encrypt_local), b(encrypt_remote),
	        xml_escape(id), b64(iv), b64(key), xml_escape(name), os, adapters_xml, region,
	        version[2], version[0], version[1], version[3]);
}

std::optional<discovery_request> parse_discovery_request(const uint8_t * data, size_t size)
{
	// one AesStream chunk per write:
	//   up to 1.34.24: RSA key XML, byte 0, byte platform, AccountID
	//   from 1.34.25:  RSA key XML, byte 1, account signature (absent before the headset got one), byte minor, byte build,
	//                  byte platform, AccountID - read from the end, whatever the signature's length
	auto chunks = aes_stream_decode_chunks(broadcast_key, broadcast_iv, data, size);
	if (!chunks || chunks->size() < 4 || (*chunks)[1].size() != 1)
		return std::nullopt;
	auto & c = *chunks;
	const size_t n = c.size();
	auto text = [](const bytes & b) { return std::string(b.begin(), b.end()); };
	if (c[1][0] == 0 && c[2].size() == 1)
		return discovery_request{text(c[0]), c[2][0], text(c[3]), std::nullopt};
	if (c[1][0] == 1 && n >= 6 && c[n - 2].size() == 1 && c[n - 3].size() == 1 && c[n - 4].size() == 1)
		return discovery_request{text(c[0]), c[n - 2][0], text(c[n - 1]), std::array<int, 4>{1, c[n - 4][0], c[n - 3][0], 0}};
	return std::nullopt;
}

std::optional<bytes> build_discovery_reply(const discovery_request & req, const computer_info & computer)
{
	auto key = random_array<32>();
	auto iv = random_array<16>();
	uint8_t key_iv[48];
	std::copy(key.begin(), key.end(), key_iv);
	std::copy(iv.begin(), iv.end(), key_iv + 32);
	auto wrapped = rsa_oaep_sha1_encrypt_xml(req.rsa_xml, key_iv, sizeof(key_iv));
	if (!wrapped)
		return std::nullopt;
	std::string xml = computer.to_xml();
	bytes body = aes_stream_writer(key, iv).encode(reinterpret_cast<const uint8_t *>(xml.data()), xml.size());
	append(*wrapped, body.data(), body.size());
	return wrapped;
}
} // namespace od
