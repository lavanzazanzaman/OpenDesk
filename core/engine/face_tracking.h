// Face and eye tracking to VRChat over OSC, built in (what VRCFaceTracking + OpenDesk's module do on Windows;
// their Windows-only named shared memory doesn't exist for VRCFaceTracking on Linux).
// The headset's FrameData face/eye state (Quest Pro etc., "Forward tracking data" on) -> VRCFaceTracking's Unified
// Expressions (OpenDesk's mapping) -> its v2 avatar parameters (shapes, simplified and combined shapes; float,
// bool or binary-encoded "Name1/2/4.. + NameNegative" as the avatar defines them) and VRChat's native eye tracking
// (/tracking/eye/...). Only parameters the current avatar has are sent, and only when they change. The avatar's
// parameters come from VRChat's OSC config files (VRChat under Proton: the game's compatdata prefix).
// Parameter definitions derived from VRCFaceTracking (Apache-2.0, github.com/benaclejames/VRCFaceTracking).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace od
{
class face_tracking
{
public:
	explicit face_tracking(std::function<void(const std::string &)> log, std::string host = "127.0.0.1", int port = 9000);
	~face_tracking();

	// a decompressed FrameData; face/eye data only when the headset sends them (size >= the face block)
	void on_frame_data(const uint8_t * data, size_t size);
	// once a second or so: avatar changes (VRChat's /avatar/change on udp 9001, newest avatar config file)
	void tick();
	// mouth and jaw: 1 = the headset's weights as they are (like OpenDesk's VRCFaceTracking module); higher
	// lifts small movements (talking) toward full ones: 1 - (1 - w)^k keeps closed at 0 and fully open at 1
	void set_mouth_sensitivity(float k);

	struct shape_data
	{
		std::array<float, 88> shapes{};
		struct eye
		{
			float gaze_x = 0, gaze_y = 0, openness = 1, pupil_mm = 5;
		} left, right;
		bool eye_active = false, expression_active = false;
	};
	// FrameData -> unified data (OpenDesk's mapping); false if the frame carries no face/eye data
	static bool unify(const uint8_t * frame_data, size_t size, shape_data & out);

private:
	struct impl;
	std::unique_ptr<impl> p;
};
} // namespace od
