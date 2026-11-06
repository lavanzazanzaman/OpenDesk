// Video channel (tcp/38830, streamer -> headset): [u8 PacketType][struct][payload], raw little-endian.
//   VideoFormat (4): 112 B    Frame (0): FrameMetadata 160 B + Annex-B access unit    Slice (1): 16 B + AU
// Layouts dumped from the real assembly (dcgen --layout); SPS/PPS travel in-band, TimestampUs must be unique.
#pragma once

#include "bytes.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace od
{
// Upstream.Net.VideoCodec wire values (the headset picks its decoder from these)
enum class video_codec : int32_t
{
	h264 = 1,
	hevc = 2,
	h264_plus = 5,
	hevc_10bit = 6,
	av1 = 10,
	av1_10bit = 11,
};
inline bool is_hevc(video_codec c) { return c == video_codec::hevc || c == video_codec::hevc_10bit; }
inline bool is_av1(video_codec c) { return c == video_codec::av1 || c == video_codec::av1_10bit; }
inline bool is_10bit(video_codec c) { return c == video_codec::hevc_10bit || c == video_codec::av1_10bit; }
const char * codec_name(video_codec);

struct rect
{
	int x = 0, y = 0, width = 0, height = 0;
	bool contains(int px, int py) const { return px >= x && py >= y && px < x + width && py < y + height; }
	bool operator==(const rect &) const = default;
};
// one streamed screen: encoded frame size (pixels) + where it sits on the desktop (Bounds; pointer coordinates)
struct stream_format
{
	int width = 0, height = 0;
	rect bounds;
	bool operator==(const stream_format &) const = default;
};
bytes pack_video_format(uint8_t source, video_codec, int fps, int width, int height, int stream_count = 1);
bytes pack_video_format(uint8_t source, video_codec, int fps, const std::vector<stream_format> & streams); // up to 4
bytes pack_slice_header(int64_t timestamp_us, int32_t stream_index, int32_t data_size);
// FrameMetadata timing, shown by the headset's performance overlay: FrameTime (PC side, pose -> encode start),
// GameTime (the app's frame time), EncodeTime. Its "Networking" figure is what is left of the total latency after them,
// so anything not reported here counts as network latency.
void set_frame_timing(bytes & frame_packet, float frame_ms, float game_ms, float encode_ms);
bytes pack_frame_header(int64_t timestamp_us, int32_t frame_index, uint8_t source, bool sync, int width, int height,
                        int32_t data_size, float frame_time_ms, float left_vibration = 0, float right_vibration = 0);

// Splits an Annex-B stream into access units without AUDs (first-slice flags / prefix NALs), see vdvideo.py.
class access_unit_splitter
{
public:
	explicit access_unit_splitter(video_codec c) :
	        hevc(is_hevc(c)) {}
	void push(const uint8_t * data, size_t size, const std::function<void(bytes &&)> & emit);
	bool pending() const { return au_start >= 0 && seen_vcl; }
	std::optional<bytes> flush(); // encoder went idle: the buffered AU is complete

private:
	bool hevc;
	bytes buf;
	ptrdiff_t au_start = -1;
	bool seen_vcl = false;
	size_t scan = 0;
};

bool is_sync(const bytes & au, video_codec);

struct desktop_video_params
{
	int fps = 60;
	int kbps = 20000;
	video_codec codec = video_codec::h264;
	bool full_range = false; // SharedUserSettings.IncreaseVideoNominalRange
	bool pre_encode = false; // StreamerSettings.Enable2Pass -> AMD pre-encode
	bool vbaq = false;       // StreamerSettings.EnableAQ -> AMD VBAQ
	bool operator==(const desktop_video_params &) const = default;
};
} // namespace od
