// Body tracking through OpenDesk XR on libovr-opendesk (harness XRBODY, the fake headset with BODY=1): XR_FB_body_tracking +
// XR_META_body_tracking_full_body joints where the headset's 32 FrameData slots belong (0-17 -> 0-17, 18-31 -> 70-83),
// the skeleton from SkeletonDataChanged, and emulated Vive trackers (XR_HTCX_vive_tracker_interaction, EmulateTrackers).
// Headless session (XR_MND_headless). Exit code 0 on success.
#include <openxr/openxr.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

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

int main()
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	uint32_t n = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
	std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
	auto has = [&](const char * e) {
		for (auto & p: props)
			if (!std::strcmp(p.extensionName, e))
				return true;
		return false;
	};
	const bool trackers = has(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);
	std::vector<const char *> ext{"XR_MND_headless", XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME};
	if (trackers)
		ext.push_back(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME);
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(ici.applicationInfo.applicationName, "xr-body-test");
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = uint32_t(ext.size());
	ici.enabledExtensionNames = ext.data();
	XrInstance instance;
	CHECK(xrCreateInstance(&ici, &instance));
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system;
	CHECK(xrGetSystem(instance, &sgi, &system));
	XrSystemPropertiesBodyTrackingFullBodyMETA full{XR_TYPE_SYSTEM_PROPERTIES_BODY_TRACKING_FULL_BODY_META};
	XrSystemBodyTrackingPropertiesFB body{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_FB, &full};
	XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES, &body};
	CHECK(xrGetSystemProperties(instance, system, &sp));
	bool ok = body.supportsBodyTracking && full.supportsFullBodyTracking;
	std::printf("%s  body tracking supported (body %u, full body %u)\n", ok ? "PASS" : "FAIL", body.supportsBodyTracking,
	            full.supportsFullBodyTracking);
	if (!ok)
		return 1;

	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = system;
	XrSession session;
	CHECK(xrCreateSession(instance, &sci, &session));
	// the body state flows once the session runs (OpenDesk XR starts its watcher in xrBeginSession)
	for (auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3); std::chrono::steady_clock::now() < end;)
	{
		XrEventDataBuffer e{XR_TYPE_EVENT_DATA_BUFFER};
		if (xrPollEvent(instance, &e) == XR_SUCCESS && e.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
		    reinterpret_cast<XrEventDataSessionStateChanged &>(e).state == XR_SESSION_STATE_READY)
			break;
	}
	XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
	sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	CHECK(xrBeginSession(session, &sbi));
	XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rci.poseInReferenceSpace.orientation.w = 1;
	XrSpace local;
	CHECK(xrCreateReferenceSpace(session, &rci, &local));

	PFN_xrCreateBodyTrackerFB create_tracker;
	PFN_xrLocateBodyJointsFB locate;
	PFN_xrGetBodySkeletonFB get_skeleton;
	CHECK(xrGetInstanceProcAddr(instance, "xrCreateBodyTrackerFB", (PFN_xrVoidFunction *)&create_tracker));
	CHECK(xrGetInstanceProcAddr(instance, "xrLocateBodyJointsFB", (PFN_xrVoidFunction *)&locate));
	CHECK(xrGetInstanceProcAddr(instance, "xrGetBodySkeletonFB", (PFN_xrVoidFunction *)&get_skeleton));
	XrBodyTrackerCreateInfoFB bci{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB};
	bci.bodyJointSet = XR_BODY_JOINT_SET_FULL_BODY_META;
	XrBodyTrackerFB tracker;
	CHECK(create_tracker(session, &bci, &tracker));

	std::vector<XrBodyJointLocationFB> joints(XR_FULL_BODY_JOINT_COUNT_META);
	XrBodyJointLocationsFB loc{XR_TYPE_BODY_JOINT_LOCATIONS_FB};
	loc.jointCount = uint32_t(joints.size());
	loc.jointLocations = joints.data();
	XrBodyJointsLocateInfoFB li{XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB};
	li.baseSpace = local;
	li.time = 1;
	// the body state comes from a watcher thread: give it a moment
	for (int i = 0; i < 50 && !(loc.isActive && loc.confidence > 0); ++i)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		CHECK(locate(tracker, &li, &loc));
	}
	std::printf("%s  joints located (active %u, confidence %.2f)\n", loc.isActive && loc.confidence > 0 ? "PASS" : "FAIL", loc.isActive,
	            loc.confidence);
	ok &= loc.isActive && loc.confidence > 0;
	// slot k sits at x = k / 100: relative to the root (slot 0), joint 17 at 0.17, joint 70 (slot 18) at 0.18, joint 83
	// (slot 31) at 0.31; the hand joints in between (no hand tracking here) untracked
	auto dx = [&](int joint) { return joints[joint].pose.position.x - joints[0].pose.position.x; };
	bool upper = std::fabs(dx(17) - 0.17f) < 1e-3f && std::fabs(dx(5) - 0.05f) < 1e-3f;
	bool lower = std::fabs(dx(70) - 0.18f) < 1e-3f && std::fabs(dx(XR_FULL_BODY_JOINT_RIGHT_FOOT_BALL_META) - 0.31f) < 1e-3f;
	bool hands_empty = !(joints[XR_FULL_BODY_JOINT_LEFT_HAND_PALM_META].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
	std::printf("%s  upper body joints 0-17 from slots 0-17 (joint 17 at +%.3f)\n", upper ? "PASS" : "FAIL", dx(17));
	std::printf("%s  lower body joints 70-83 from slots 18-31 (left upper leg +%.3f, right foot ball +%.3f)\n", lower ? "PASS" : "FAIL",
	            dx(70), dx(XR_FULL_BODY_JOINT_RIGHT_FOOT_BALL_META));
	std::printf("%s  hand joints untracked without hand tracking\n", hands_empty ? "PASS" : "FAIL");
	ok &= upper && lower && hands_empty;
	std::printf("%s  skeleton changed count %u\n", loc.skeletonChangedCount == 7 ? "PASS" : "FAIL", loc.skeletonChangedCount);
	ok &= loc.skeletonChangedCount == 7;

	std::vector<XrBodySkeletonJointFB> skel(XR_FULL_BODY_JOINT_COUNT_META);
	XrBodySkeletonFB sk{XR_TYPE_BODY_SKELETON_FB};
	sk.jointCount = uint32_t(skel.size());
	sk.joints = skel.data();
	CHECK(get_skeleton(tracker, &sk));
	bool skeleton = skel[40].joint == 40 && skel[40].parentJoint == 39 && std::fabs(skel[40].pose.position.y - 0.40f) < 1e-4f &&
	                skel[83].joint == 83;
	std::printf("%s  skeleton from SkeletonDataChanged (joint 40: parent %d, y %.2f)\n", skeleton ? "PASS" : "FAIL", skel[40].parentJoint,
	            skel[40].pose.position.y);
	ok &= skeleton;

	if (trackers)
	{
		PFN_xrEnumerateViveTrackerPathsHTCX enum_paths;
		CHECK(xrGetInstanceProcAddr(instance, "xrEnumerateViveTrackerPathsHTCX", (PFN_xrVoidFunction *)&enum_paths));
		uint32_t count = 0;
		CHECK(enum_paths(instance, 0, &count, nullptr));
		std::printf("%s  emulated Vive trackers (%u)\n", count > 0 ? "PASS" : "FAIL", count);
		ok &= count > 0;
	}
	else
		std::printf("FAIL  runtime lacks %s\n", XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME), ok = false;
	xrRequestExitSession(session);
	xrDestroySession(session);
	xrDestroyInstance(instance);
	std::printf(ok ? "ALL PASS\n" : "FAILED\n");
	return ok ? 0 : 1;
}
