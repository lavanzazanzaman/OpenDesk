#include "video.h"

#include <algorithm>

#include <cstring>

namespace od
{
namespace
{
constexpr uint8_t packet_frame = 0;
constexpr uint8_t packet_slice = 1;
constexpr uint8_t packet_video_format = 4;
constexpr uint8_t source_desktop = 0;


struct nal_class
{
	bool vcl, first_slice, prefix;
};

nal_class classify(bool hevc, const uint8_t * h)
{
	if (hevc)
	{
		int t = (h[0] >> 1) & 0x3f;
		if (t < 32)
			return {true, bool(h[2] & 0x80), false}; // first_slice_segment_in_pic_flag
		return {false, false, t == 32 || t == 33 || t == 34 || t == 35 || t == 39 || (t >= 41 && t <= 44) || (t >= 48 && t <= 55)};
	}
	int t = h[0] & 0x1f;
	if (t >= 1 && t <= 5)
		return {true, bool(h[1] & 0x80), false}; // first_mb_in_slice == 0 (ue(v) '1')
	return {false, false, t == 6 || t == 7 || t == 8 || t == 9 || (t >= 14 && t <= 18)};
}

const uint8_t * find_start(const uint8_t * p, const uint8_t * end)
{
	static const uint8_t sc[3] = {0, 0, 1};
	auto it = std::search(p, end, sc, sc + 3);
	return it == end ? nullptr : it;
}
} // namespace

const char * codec_name(video_codec c)
{
	switch (c)
	{
		case video_codec::h264: return "H.264";
		case video_codec::h264_plus: return "H.264+";
		case video_codec::hevc: return "HEVC";
		case video_codec::hevc_10bit: return "HEVC 10-bit";
		case video_codec::av1: return "AV1";
		case video_codec::av1_10bit: return "AV1 10-bit";
	}
	return "?";
}

bytes pack_video_format(uint8_t source, video_codec codec, int fps, int width, int height, int stream_count)
{
	bytes b;
	b.reserve(113);
	b.push_back(packet_video_format);
	b.push_back(source);
	b.insert(b.end(), 3, 0);
	for (int32_t v: {int32_t(codec), int32_t(fps), int32_t(width), int32_t(height), int32_t(stream_count), 0, 0, width, height})
		append_le(b, v); // ... StreamCount, Bounds{0,0,w,h}
	b.resize(113, 0);    // Secondary/Tertiary/Quaternary sizes and bounds
	return b;
}

bytes pack_video_format(uint8_t source, video_codec codec, int fps, const std::vector<stream_format> & streams)
{
	// VideoFormat: Source u8 (+3), Codec, FrameRate, FrameSize, StreamCount, Bounds,
	//              {Secondary,Tertiary,Quaternary}{FrameSize, Bounds}
	bytes b;
	b.reserve(113);
	b.push_back(packet_video_format);
	b.push_back(source);
	b.insert(b.end(), 3, 0);
	auto size_and_bounds = [&](const stream_format & s) {
		for (int32_t v: {s.width, s.height, s.bounds.x, s.bounds.y, s.bounds.width, s.bounds.height})
			append_le(b, v);
	};
	const stream_format primary = streams.empty() ? stream_format{} : streams[0];
	for (int32_t v: {int32_t(codec), int32_t(fps), primary.width, primary.height, int32_t(std::min<size_t>(streams.size(), 4))})
		append_le(b, v);
	for (int32_t v: {primary.bounds.x, primary.bounds.y, primary.bounds.width, primary.bounds.height})
		append_le(b, v);
	for (size_t i = 1; i < 4; ++i)
		size_and_bounds(i < streams.size() ? streams[i] : stream_format{});
	return b;
}

bytes pack_slice_header(int64_t ts, int32_t stream_index, int32_t size)
{
	bytes b;
	b.reserve(17);
	b.push_back(packet_slice);
	append_le(b, ts);
	append_le(b, stream_index);
	append_le(b, size);
	return b;
}

void set_frame_timing(bytes & frame_packet, float frame_ms, float game_ms, float encode_ms)
{
	if (frame_packet.size() < 161)
		return;
	uint8_t * m = frame_packet.data() + 1;
	std::memcpy(m + 16, &frame_ms, 4);
	std::memcpy(m + 20, &game_ms, 4);
	std::memcpy(m + 24, &encode_ms, 4);
}

bytes pack_frame_header(int64_t ts, int32_t index, uint8_t source, bool sync, int width, int height, int32_t size, float frame_time_ms,
                        float left_vibration, float right_vibration)
{
	bytes b(161, 0);
	b[0] = packet_frame;
	uint8_t * m = b.data() + 1;
	std::memcpy(m + 0, &ts, 8);
	std::memcpy(m + 8, &index, 4);
	m[12] = source;
	m[13] = sync;
	std::memcpy(m + 16, &frame_time_ms, 4);
	// RenderPose (28..119) stays zero: HasValidPose=false for desktop
	std::memcpy(m + 120, &left_vibration, 4);  // LeftVibration.Amplitude (XInput: left = strong motor)
	std::memcpy(m + 132, &right_vibration, 4); // RightVibration.Amplitude
	int32_t wh[2] = {width, height};
	std::memcpy(m + 144, wh, 8); // RenderResolution
	std::memcpy(m + 152, &size, 4);
	return b;
}

void access_unit_splitter::push(const uint8_t * data, size_t size, const std::function<void(bytes &&)> & emit)
{
	append(buf, data, size);
	size_t i = scan;
	while (true)
	{
		const uint8_t * p = find_start(buf.data() + i, buf.data() + buf.size());
		if (!p)
			break;
		size_t j = size_t(p - buf.data());
		if (j + 6 > buf.size())
			break;
		size_t nal_start = (j > 0 && buf[j - 1] == 0) ? j - 1 : j;
		nal_class c = classify(hevc, buf.data() + j + 3);
		bool new_au = seen_vcl && (c.prefix || (c.vcl && c.first_slice));
		if (au_start < 0)
			au_start = ptrdiff_t(nal_start);
		else if (new_au)
		{
			emit(bytes(buf.begin() + au_start, buf.begin() + ptrdiff_t(nal_start)));
			au_start = ptrdiff_t(nal_start);
			seen_vcl = false;
		}
		if (c.vcl)
			seen_vcl = true;
		i = j + 3;
	}
	if (au_start > 0) // compact
	{
		buf.erase(buf.begin(), buf.begin() + au_start);
		i -= size_t(au_start);
		au_start = 0;
	}
	scan = i;
}

std::optional<bytes> access_unit_splitter::flush()
{
	if (!pending())
		return std::nullopt;
	bytes au(buf.begin() + au_start, buf.end());
	buf.clear();
	au_start = -1;
	seen_vcl = false;
	scan = 0;
	return au;
}

bool is_sync(const bytes & au, video_codec codec)
{
	const uint8_t * p = au.data();
	const uint8_t * end = au.data() + au.size();
	while ((p = find_start(p, end)) && p + 3 < end)
	{
		if (is_hevc(codec))
		{
			int t = (p[3] >> 1) & 0x3f;
			if (t >= 16 && t <= 21) // BLA/IDR/CRA
				return true;
		}
		else if ((p[3] & 0x1f) == 5)
			return true;
		p += 3;
	}
	return false;
}

} // namespace od
