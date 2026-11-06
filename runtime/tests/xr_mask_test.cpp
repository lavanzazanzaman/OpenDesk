// The FOV stencil through OpenDesk XR (XR_KHR_visibility_mask), headless session. xr-mask-test [off]
//   on: the hidden mesh covers the image corners, not the centre; the visible mesh the centre
//   off ("Use Fov Stencil" unchecked): empty masks
#include <openxr/openxr.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

namespace
{
bool inside(XrVector2f p, XrVector2f a, XrVector2f b, XrVector2f c)
{
	auto cross = [](XrVector2f o, XrVector2f u, XrVector2f v) { return (u.x - o.x) * (v.y - o.y) - (u.y - o.y) * (v.x - o.x); };
	if (std::fabs(cross(a, b, c)) < 1e-9f)
		return false; // degenerate
	float d1 = cross(p, a, b), d2 = cross(p, b, c), d3 = cross(p, c, a);
	bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
	return !(neg && pos);
}
bool covered(XrVector2f p, const std::vector<XrVector2f> & v, const std::vector<uint32_t> & idx)
{
	for (size_t i = 0; i + 2 < idx.size(); i += 3)
		if (inside(p, v[idx[i]], v[idx[i + 1]], v[idx[i + 2]]))
			return true;
	return false;
}
} // namespace

int main(int argc, char ** argv)
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const bool off = argc > 1 && !std::strcmp(argv[1], "off");
	const char * ext[] = {"XR_MND_headless", XR_KHR_VISIBILITY_MASK_EXTENSION_NAME};
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(ici.applicationInfo.applicationName, "xr-mask-test");
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = ext;
	XrInstance instance;
	CHECK(xrCreateInstance(&ici, &instance));
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId system;
	CHECK(xrGetSystem(instance, &sgi, &system));
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = system;
	XrSession session;
	CHECK(xrCreateSession(instance, &sci, &session));
	PFN_xrGetVisibilityMaskKHR get_mask;
	CHECK(xrGetInstanceProcAddr(instance, "xrGetVisibilityMaskKHR", (PFN_xrVoidFunction *)&get_mask));
	// the view's FOV (the mask is in its tangent-space plane at z = -1)
	XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	uint32_t nv = 0;
	CHECK(xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, views));
	bool ok = true;
	for (uint32_t eye = 0; eye < 2; ++eye)
	{
		auto fetch = [&](XrVisibilityMaskTypeKHR type, std::vector<XrVector2f> & v, std::vector<uint32_t> & idx) -> XrResult {
			XrVisibilityMaskKHR m{XR_TYPE_VISIBILITY_MASK_KHR};
			XrResult r = get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, type, &m);
			if (XR_FAILED(r))
				return r;
			v.resize(m.vertexCountOutput);
			idx.resize(m.indexCountOutput);
			m.vertexCapacityInput = uint32_t(v.size());
			m.indexCapacityInput = uint32_t(idx.size());
			m.vertices = v.data();
			m.indices = idx.data();
			return v.empty() ? XR_SUCCESS : get_mask(session, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, type, &m);
		};
		std::vector<XrVector2f> hv, vv, lv;
		std::vector<uint32_t> hi, vi, li;
		CHECK(fetch(XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, hv, hi));
		CHECK(fetch(XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, vv, vi));
		CHECK(fetch(XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, lv, li));
		if (off)
		{
			bool empty = hv.empty() && vv.empty() && lv.empty();
			std::printf("%s  eye %u: stencil off -> empty masks (%zu / %zu / %zu vertices)\n", empty ? "PASS" : "FAIL", eye, hv.size(), vv.size(),
			            lv.size());
			ok &= empty;
			continue;
		}
		// the image corners in that plane: well inside the hidden area; the centre: visible
		float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
		for (auto & p: hv)
			minx = std::min(minx, p.x), maxx = std::max(maxx, p.x), miny = std::min(miny, p.y), maxy = std::max(maxy, p.y);
		XrVector2f corner{minx + 0.02f * (maxx - minx), maxy - 0.02f * (maxy - miny)}, centre{0, 0};
		if (std::getenv("XR_MASK_DEBUG"))
		{
			std::printf("      bounds x %.3f..%.3f y %.3f..%.3f; hidden v0-3:", minx, maxx, miny, maxy);
			for (int i = 0; i < 4 && i < int(hv.size()); ++i)
				std::printf(" (%.3f,%.3f)", hv[i].x, hv[i].y);
			std::printf("; visible v0-2:");
			for (int i = 0; i < 3 && i < int(vv.size()); ++i)
				std::printf(" (%.3f,%.3f)", vv[i].x, vv[i].y);
			std::printf("; idx0-5:");
			for (int i = 0; i < 6 && i < int(hi.size()); ++i)
				std::printf(" %u", hi[i]);
			std::printf("\n");
		}
		bool hidden_ok = !hv.empty() && covered(corner, hv, hi) && !covered(centre, hv, hi);
		bool visible_ok = !vv.empty() && covered(centre, vv, vi) && !covered(corner, vv, vi);
		std::printf("%s  eye %u: hidden mesh %zu vertices / %zu triangles covers the corner, not the centre\n", hidden_ok ? "PASS" : "FAIL", eye,
		            hv.size(), hi.size() / 3);
		std::printf("%s  eye %u: visible mesh covers the centre, not the corner; line loop %zu points\n",
		            visible_ok && !lv.empty() ? "PASS" : "FAIL", eye, lv.size());
		ok &= hidden_ok && visible_ok && !lv.empty();
	}
	xrDestroySession(session);
	xrDestroyInstance(instance);
	std::printf(ok ? "ALL PASS\n" : "FAILED\n");
	return ok ? 0 : 1;
}
