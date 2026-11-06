#include "vr_types.h"

#include <algorithm>
#include <cstring>

namespace od
{
bytes pack_vr_video_format(video_codec codec, int fps, int enc_w, int enc_h, int eye_w, int eye_h)
{
	// primary: encoded size + Bounds = eye size; secondary (right eye): encoded size
	std::vector<stream_format> f{{enc_w, enc_h, {0, 0, eye_w, eye_h}}, {enc_w, enc_h, {0, 0, 0, 0}}};
	return pack_video_format(source_vr, codec, fps, f);
}

bytes pack_vr_video_format_foveated(video_codec codec, int fps, int pw, int ph, int fw, int fh, int eye_w, int eye_h)
{
	std::vector<stream_format> f{{pw, ph, {0, 0, eye_w, eye_h}}, {fw, fh, {0, 0, 0, 0}}, {pw, ph, {0, 0, 0, 0}}, {fw, fh, {0, 0, 0, 0}}};
	return pack_video_format(source_vr, codec, fps, f);
}

bytes pack_vr_frame_header(int64_t ts, const vr_render_info & info, bool sync, int eye_w, int eye_h, int32_t size,
                           float frame_time_ms, float left_vibration, float right_vibration)
{
	bytes b = pack_frame_header(ts, info.pose_index, source_vr, sync, eye_w, eye_h, size, frame_time_ms, left_vibration,
	                            right_vibration);
	uint8_t * m = b.data() + 1;
	std::memcpy(m + 20, &frame_time_ms, 4); // GameTime
	// Left/RightVibration (@120/@132): {float Amplitude; Point FoveaTopLeft} - the foveated streaming inset positions
	std::memcpy(m + 124, info.fovea_top_left[0], 8);
	std::memcpy(m + 136, info.fovea_top_left[1], 8);
	// RenderPose @28: HasValidPose, HasLeft, HasRight, pad; left quat+pos; right quat+pos; FovLeft; FovRight
	m[28] = m[29] = m[30] = 1;
	float * f = reinterpret_cast<float *>(m + 32);
	for (int e = 0; e < 2; ++e)
	{
		std::memcpy(f, info.eye[e].orientation.data(), 16);
		std::memcpy(f + 4, info.eye[e].position.data(), 12);
		f += 7;
	}
	for (int e = 0; e < 2; ++e)
	{
		const fov_port & p = info.fov[e];
		float v[4] = {p.up, p.down, p.left, p.right};
		std::memcpy(f, v, 16);
		f += 4;
	}
	return b;
}

void vr_eye_resolution(int hmd, int quality, int & w, int & h)
{
	struct size
	{
		int w, h;
	};
	// index = quality + 1 (Potato .. Unstoppable)
	static constexpr size quest1[] = {{1200, 1344}, {1536, 1728}, {1824, 2016}, {2208, 2400}};
	static constexpr size quest[] = {{1344, 1440}, {1728, 1824}, {2112, 2304}, {2496, 2688}, {2688, 2880},
	                                 {3072, 3264}, {3648, 4032}, {4224, 4608}, {4992, 5376}};
	// PlayForDream MR, Pico Space Pro, Meta VR Glasses: wider than tall
	static constexpr size wide[] = {{1536, 1440}, {1824, 1728}, {2208, 2016}, {2688, 2496}, {2880, 2688},
	                                {3264, 3072}, {4032, 3648}, {4608, 4224}, {5376, 4992}};
	int i = std::clamp(quality + 1, 0, 8);
	size s = hmd == 259 ? quest1[std::min(i, 3)] : (hmd == 1400 || hmd == 1700 || hmd == 1710) ? wide[i] : quest[i];
	w = s.w;
	h = s.h;
}

int64_t vr_max_bitrate(int hmd, video_codec codec)
{
	switch (hmd)
	{
		case 259: return 100000000;
		case 320: case 400: case 600: case 650: case 700: case 701: case 800: return codec == video_codec::h264_plus ? 400000000 : 150000000;
		case 1000: case 1001: return codec == video_codec::h264_plus ? 500000000 : 200000000;
		case 1700: case 1710: return codec == video_codec::h264_plus ? 800000000 : is_hevc(codec) ? 400000000 : 200000000;
		case 1010: case 1100: case 1400: case 1500: case 1600: return codec == video_codec::h264_plus ? 600000000 : 200000000;
		default: return 40000000;
	}
}
} // namespace od
