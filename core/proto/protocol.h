// upstream wire protocol (reverse-engineered from the headset app, upstream 1.34.22; 1.34.23 is the same on the wire).
// Reference implementation: engine/odproto.py (verified against a Quest 3).
#pragma once

#include "crypto.h"

#include <string>
#include <vector>

namespace od
{
constexpr uint16_t discovery_port = 38850;
constexpr uint16_t messaging_port = 38810;
constexpr uint16_t data_port = 38820;
constexpr uint16_t video_port = 38830;
constexpr uint16_t audio_port = 38840;

// ComputerDiscoveryClient: static key used for the broadcast request
extern const aes_key broadcast_key;
extern const aes_iv broadcast_iv;

enum class message_type : uint8_t
{
	peer_info = 1,
	update_required = 4,
	update_unavailable = 5,
	user_settings_reload = 11,
	user_settings_property_changed = 12,
	mobile_settings_reload = 13,
	mobile_settings_property_changed = 14,
	streamer_settings_reload = 15,
	streamer_settings_property_changed = 16,
	skeleton_data_changed = 22, // body tracking skeleton (XR_FB_body_tracking T-pose): joints x 36 bytes + i32 changed count
	restart_stream = 23,
	screenshot_to_desktop = 70,
	change_language = 80, // headset keyboard's language key: next keyboard layout
	force_key_frame = 24,
	switch_monitor = 25,
	toggle_dictation = 26, // headset keyboard's microphone key (it also turns Microphone passthrough on)
	recenter = 29,
	request_games = 30,
	games = 31,
	request_game_thumbnails = 32,
	game_thumbnail = 33,
	launch_game = 34,
	exit_game = 35,
	game_exited = 36,
	add_monitor = 42,
	remove_monitor = 43,
	set_monitor_portrait = 44,
	request_videos = 50,
	videos = 51,
	request_video_thumbnails = 52,
	video_thumbnail = 53,
	play_video = 54,
	download_video = 55,
	stop_video = 56,
	video_stopped = 57,
	seek_video = 58,
	cancel_download_video = 59,
	audio_tracks = 60,
	set_audio_stream_index = 61,
	paste_url = 62,
	cancel_paste_url = 63,
	video_added = 64,
	subtitle_tracks = 65,
	set_subtitle_track_id = 66,
	set_subtitle_stream_index = 67,
	set_subtitle_offset_amount = 68,
	disconnect = 81,
};
const char * message_name(uint8_t type);

// NetMessage: u8 type, i32 big-endian payload length, payload
bytes encode_net_message(message_type, const uint8_t * payload, size_t size);
inline bytes encode_net_message(message_type t, const std::string & payload)
{
	return encode_net_message(t, reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
}

struct net_message
{
	uint8_t type;
	bytes payload;
};
// pops one complete message from the front of a plaintext buffer
std::optional<net_message> pop_net_message(bytes & plaintext);

struct network_adapter
{
	std::string ip;
	std::string mac;
	bool is_wireless = false;
	bool is_gigabit = false;
	bool is_usb = false;
};

// The headset app refuses a streamer whose minor version is higher than its own, and asks older
// streamers to update. 1.20.3 is the lowest Windows version it does not reject as a manual update,
// so advertising it keeps every headset app from 1.20.3 onward connectable. Newer versions unlock
// headset-side features (eye data >= 1.34.15, stream restart >= 1.34.20) but not the connection.
inline constexpr std::array<int, 4> default_streamer_version{1, 20, 3, 0};
inline constexpr std::array<int, 4> latest_known_streamer_version{1, 34, 23, 0};

struct computer_info
{
	std::string id;
	std::string name;
	std::string description;
	std::vector<network_adapter> adapters;
	std::array<uint8_t, 16> connection_id;
	aes_key key;
	aes_iv iv;
	// Reported to the headset in discovery. Overridable at runtime (compatibility mode is the default).
	std::array<int, 4> version = default_streamer_version;
	std::string os = "Windows";             // enum OS { Windows, MacOS }
	std::string region = "Europe";
	bool allow_remote = false;
	bool encrypt_local = false;
	bool encrypt_remote = true;

	// DataContractSerializer XML; element order verified against the real assembly
	std::string to_xml() const;
};

std::string version_string(const std::array<int, 4> & v);
// "1.34.23", "1.34.23.0"; returns default_streamer_version when the text has no leading number
std::array<int, 4> parse_streamer_version(std::string_view text);

struct discovery_request
{
	std::string rsa_xml;
	int platform;
	std::string account_id;
};
std::optional<discovery_request> parse_discovery_request(const uint8_t * data, size_t size);
// RSA-OAEP-SHA1(replyKey32 + replyIV16) [128 B] + AesStream(replyKey, replyIV, Computer XML)
std::optional<bytes> build_discovery_reply(const discovery_request &, const computer_info &);
} // namespace od
