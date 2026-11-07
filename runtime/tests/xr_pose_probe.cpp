// Controller grip pose relative to the aim pose, as the active OpenXR runtime reports them (headless session).
//   xr-pose-probe [seconds]
// Run it under two runtimes with the controllers held still to compare their grip/aim conventions, e.g. WiVRn
// (the headset's own poses) against OpenDesk XR (grip derived from the aim pose).
#include <ctime>
#define XR_USE_TIMESPEC
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#define CHECK(x)                                                                                                       \
	do                                                                                                                 \
	{                                                                                                                  \
		XrResult r_ = (x);                                                                                             \
		if (XR_FAILED(r_))                                                                                             \
		{                                                                                                              \
			std::printf("FAIL  %s -> %d\n", #x, int(r_));                                                             \
			return 1;                                                                                                  \
		}                                                                                                              \
	} while (0)

namespace
{
// rotation as XYZ Euler angles in degrees (x: pitch about the controller's right axis)
void euler(const XrQuaternionf & q, double & x, double & y, double & z)
{
	const double sinr = 2 * (q.w * q.x + q.y * q.z), cosr = 1 - 2 * (q.x * q.x + q.y * q.y);
	x = std::atan2(sinr, cosr);
	const double sinp = 2 * (q.w * q.y - q.z * q.x);
	y = std::fabs(sinp) >= 1 ? std::copysign(M_PI / 2, sinp) : std::asin(sinp);
	const double siny = 2 * (q.w * q.z + q.x * q.y), cosy = 1 - 2 * (q.y * q.y + q.z * q.z);
	z = std::atan2(siny, cosy);
	x *= 180 / M_PI;
	y *= 180 / M_PI;
	z *= 180 / M_PI;
}
} // namespace

int main(int argc, char ** argv)
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const int seconds = argc > 1 ? std::atoi(argv[1]) : 20;
	const char * ext[] = {"XR_MND_headless", "XR_KHR_convert_timespec_time"};
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(ici.applicationInfo.applicationName, "xr-pose-probe");
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = ext;
	XrInstance instance;
	CHECK(xrCreateInstance(&ici, &instance));
	XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
	xrGetInstanceProperties(instance, &props);
	std::printf("runtime: %s\n", props.runtimeName);
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system;
	CHECK(xrGetSystem(instance, &sgi, &system));
	PFN_xrConvertTimespecTimeToTimeKHR to_xr_time;
	CHECK(xrGetInstanceProcAddr(instance, "xrConvertTimespecTimeToTimeKHR", (PFN_xrVoidFunction *)&to_xr_time));

	XrPath hands[2], grip_paths[2], aim_paths[2];
	const char * names[2] = {"left", "right"};
	for (int h = 0; h < 2; ++h)
	{
		char p[128];
		std::snprintf(p, sizeof p, "/user/hand/%s", names[h]);
		CHECK(xrStringToPath(instance, p, &hands[h]));
		std::snprintf(p, sizeof p, "/user/hand/%s/input/grip/pose", names[h]);
		CHECK(xrStringToPath(instance, p, &grip_paths[h]));
		std::snprintf(p, sizeof p, "/user/hand/%s/input/aim/pose", names[h]);
		CHECK(xrStringToPath(instance, p, &aim_paths[h]));
	}
	XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(asci.actionSetName, "probe");
	std::strcpy(asci.localizedActionSetName, "Probe");
	XrActionSet set;
	CHECK(xrCreateActionSet(instance, &asci, &set));
	auto pose_action = [&](const char * name, XrAction & a) -> XrResult {
		XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
		aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
		std::strcpy(aci.actionName, name);
		std::strcpy(aci.localizedActionName, name);
		aci.countSubactionPaths = 2;
		aci.subactionPaths = hands;
		return xrCreateAction(set, &aci, &a);
	};
	XrAction grip, aim;
	CHECK(pose_action("grip", grip));
	CHECK(pose_action("aim", aim));
	XrPath profile;
	CHECK(xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &profile));
	XrActionSuggestedBinding b[4] = {{grip, grip_paths[0]}, {grip, grip_paths[1]}, {aim, aim_paths[0]}, {aim, aim_paths[1]}};
	XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	sb.interactionProfile = profile;
	sb.countSuggestedBindings = 4;
	sb.suggestedBindings = b;
	CHECK(xrSuggestInteractionProfileBindings(instance, &sb));

	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = system;
	XrSession session;
	CHECK(xrCreateSession(instance, &sci, &session));
	XrSessionActionSetsAttachInfo att{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	att.countActionSets = 1;
	att.actionSets = &set;
	CHECK(xrAttachSessionActionSets(session, &att));
	XrSpace grip_space[2], aim_space[2];
	for (int h = 0; h < 2; ++h)
	{
		XrActionSpaceCreateInfo s{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		s.poseInActionSpace.orientation.w = 1;
		s.subactionPath = hands[h];
		s.action = grip;
		CHECK(xrCreateActionSpace(session, &s, &grip_space[h]));
		s.action = aim;
		CHECK(xrCreateActionSpace(session, &s, &aim_space[h]));
	}

	bool running = false;
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	auto next_print = std::chrono::steady_clock::now();
	while (std::chrono::steady_clock::now() < end)
	{
		XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(instance, &ev) == XR_SUCCESS)
		{
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			{
				auto * st = reinterpret_cast<XrEventDataSessionStateChanged *>(&ev);
				if (st->state == XR_SESSION_STATE_READY && !running)
				{
					XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					CHECK(xrBeginSession(session, &bi));
					running = true;
				}
			}
			ev = {XR_TYPE_EVENT_DATA_BUFFER};
		}
		if (running)
		{
			XrActiveActionSet active{set, XR_NULL_PATH};
			XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
			si.countActiveActionSets = 1;
			si.activeActionSets = &active;
			xrSyncActions(session, &si);
			if (std::chrono::steady_clock::now() >= next_print)
			{
				next_print += std::chrono::milliseconds(1000);
				timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				XrTime now = 0;
				to_xr_time(instance, &ts, &now);
				for (int h = 0; h < 2; ++h)
				{
					XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
					if (XR_FAILED(xrLocateSpace(grip_space[h], aim_space[h], now, &loc)) ||
					    !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
					{
						std::printf("%-5s: not tracked\n", names[h]);
						continue;
					}
					double x, y, z;
					euler(loc.pose.orientation, x, y, z);
					const double angle = 2 * std::acos(std::fmin(1.0, std::fabs(loc.pose.orientation.w))) * 180 / M_PI;
					std::printf("%-5s: grip in aim space: rotation %.1f deg (x %.1f, y %.1f, z %.1f), quat (%.4f %.4f %.4f %.4f), "
					            "position (%.1f %.1f %.1f) mm\n",
					            names[h], angle, x, y, z, loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z,
					            loc.pose.orientation.w, loc.pose.position.x * 1000, loc.pose.position.y * 1000,
					            loc.pose.position.z * 1000);
				}
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	xrDestroyInstance(instance);
	return 0;
}
