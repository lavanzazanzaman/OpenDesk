// A running desktop video stream on the video socket (portal capture; later: X11 capture, VR).
#pragma once

namespace od
{
class video_source
{
public:
	virtual ~video_source() = default;
	virtual bool running() const = 0;
	virtual void request_keyframe() = 0; // ForceKeyFrame / RestartStream from the headset
	// gamepad rumble (0..1): carried to the headset in FrameMetadata Left/RightVibration.Amplitude
	virtual void set_vibration(float strong, float weak) = 0;
};
} // namespace od
