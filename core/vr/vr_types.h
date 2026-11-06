// VR stream basics shared by the streamer and (later) the OpenXR runtime side.
//   VideoFormat: Source=VR(1), StreamCount 2 (s0 left eye, s1 right eye), Bounds = eye render size,
//                FrameSize = SecondaryFrameSize = encoded size (eye / 1.5, defoveation-warped)
//   per frame  : Frame packet (left AU, FrameMetadata with RenderPose, FrameIndex = headset pose frame) +
//                Slice packet (StreamIndex 1, right AU) with the same TimestampUs
// Defoveation (headset DefoveationPrimitive): screen s -> texture t: s<.25: 2s^2; s>.75: 1-2(1-s)^2; else
// 1.5(s-.25)+.125. We render texture pixels, sampling screen position s = t^-1.
#pragma once

#include "bytes.h"
#include "video.h"

#include <array>
#include <cstdint>

namespace od
{
constexpr uint8_t source_vr = 1;
constexpr double defoveation_ratio = 1.5;

struct vr_pose
{
	std::array<float, 4> orientation{0, 0, 0, 1}; // x, y, z, w
	std::array<float, 3> position{0, 1.6f, 0};
};

struct fov_port // tangents, all positive (OVR FovPort order: up, down, left, right)
{
	float up = 1, down = 1, left = 1, right = 1;
};

struct vr_render_info
{
	int32_t pose_index = 0; // FrameData.FrameIndex the poses were taken from
	vr_pose eye[2];
	fov_port fov[2];
	int32_t fovea_top_left[2][2] = {}; // foveated streaming: where each eye's fovea image sits in the eye render target
};

bytes pack_vr_video_format(video_codec, int fps, int encoded_width, int encoded_height, int eye_width, int eye_height);
// foveated streaming (eye-tracked headsets): StreamCount 4 - s0 left periphery, s1 left fovea, s2 right periphery,
// s3 right fovea; Bounds = eye render size (the headset places the fovea 1:1 at FrameMetadata.*Vibration.FoveaTopLeft)
bytes pack_vr_video_format_foveated(video_codec, int fps, int periphery_width, int periphery_height, int fovea_width, int fovea_height,
                                    int eye_width, int eye_height);
bytes pack_vr_frame_header(int64_t timestamp_us, const vr_render_info &, bool sync, int eye_width, int eye_height,
                           int32_t data_size, float frame_time_ms, float left_vibration = 0, float right_vibration = 0);

// Xenko.VR.HmdResolutionTypeExtensions (headset): per-eye render size for a VRGraphicsQuality (-1 Potato .. 7)
void vr_eye_resolution(int hmd_type, int quality, int & width, int & height);
// max VR bitrate (bit/s) for the headset and codec; the headset's VRBitrateLimit lerps from 10 Mbps to it
int64_t vr_max_bitrate(int hmd_type, video_codec);
} // namespace od
