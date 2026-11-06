// OpenXR smoke test without graphics: instance, HMD system and its properties through whatever runtime the loader picks
// (XR_RUNTIME_JSON for the build's manifest). Exercises the loader handshake, libovr-opendesk's connection to the
// streamer and the runtime's system queries.
//   xr-system-test
#include <openxr/openxr.h>

#include <cstdio>
#include <cstring>

#define CHECK(x)                                                                                                       \
	do                                                                                                                 \
	{                                                                                                                  \
		XrResult r_ = (x);                                                                                             \
		if (XR_FAILED(r_))                                                                                             \
		{                                                                                                              \
			std::printf("FAIL %s: %d\n", #x, int(r_));                                                                 \
			return 1;                                                                                                  \
		}                                                                                                              \
	} while (0)

int main()
{
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(ici.applicationInfo.applicationName, "xr-system-test");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	XrInstance instance = XR_NULL_HANDLE;
	CHECK(xrCreateInstance(&ici, &instance));
	XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
	CHECK(xrGetInstanceProperties(instance, &ip));
	std::printf("runtime: %s\n", ip.runtimeName);
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	CHECK(xrGetSystem(instance, &sgi, &system));
	XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
	CHECK(xrGetSystemProperties(instance, system, &sp));
	std::printf("system: %s, max layers %u, orientation tracking %u\n", sp.systemName, sp.graphicsProperties.maxLayerCount,
	            sp.trackingProperties.orientationTracking);
	uint32_t views = 0;
	CHECK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &views, nullptr));
	XrViewConfigurationView v[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	CHECK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &views, v));
	std::printf("views: %u, recommended %ux%u\n", views, v[0].recommendedImageRectWidth, v[0].recommendedImageRectHeight);
	xrDestroyInstance(instance);
	std::printf("PASS\n");
	return 0;
}
