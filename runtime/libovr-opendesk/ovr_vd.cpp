// libovr-opendesk: the LibOVR C API (the subset OpenDesk's OpenXR runtime OpenDesk XR uses) implemented on top of the
// Linux streamer's VR interface (vr_client): tracking from the shared FrameData, Vulkan swapchains on the app's
// device, and a compositor that writes each frame straight into the streamer's encoder buffers.
#include "compositor.h"
#include "frame_data.h"
#include "math.h"
#include "vr_client.h"

#include <OVR_CAPI.h>
#include <OVR_CAPI_Vk.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unistd.h>
#define OVRVD_EXPORT extern "C" __attribute__((visibility("default")))

using namespace od;
using namespace od::ovr;

namespace od::ovr
{
// messages worth seeing in the streamer's log (errors, status changes, stream setup): stderr and, through the
// newest session's connection, the streamer; lines from before a session connected wait there
namespace
{
std::mutex report_mutex;
vr_client * report_client = nullptr;
std::vector<std::string> report_backlog;
} // namespace

void report_line(const std::string & line)
{
	std::fprintf(stderr, "[libovr-opendesk] %s\n", line.c_str());
	std::lock_guard lock(report_mutex);
	if (report_client)
		report_client->log(line);
	else if (report_backlog.size() < 50)
		report_backlog.push_back(line);
}

void report(const char * fmt, ...)
{
	char buf[480];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	report_line(buf);
}

void set_report_client(vr_client * c)
{
	std::lock_guard lock(report_mutex);
	report_client = c;
	if (c)
	{
		for (const auto & line: report_backlog)
			c->log(line);
		report_backlog.clear();
	}
}
} // namespace od::ovr

using od::ovr::report;

namespace
{
double now_seconds()
{
	return double(ipc::monotonic_ns()) * 1e-9; // the streamer's clock (CLOCK_MONOTONIC / QueryPerformanceCounter)
}

void log(const char * fmt, ...)
{
	static const bool enabled = getenv("OPENDESK_OVR_LOG") != nullptr;
	if (!enabled)
		return;
	va_list ap;
	va_start(ap, fmt);
	std::fprintf(stderr, "[libovr-opendesk] ");
	std::vfprintf(stderr, fmt, ap);
	std::fprintf(stderr, "\n");
	va_end(ap);
}

std::string last_error;
std::mutex global_mutex;
bool initialized = false;

ovrResult fail(ovrResult r, const std::string & message)
{
	last_error = message;
	report("error: %s", message.c_str());
	return r;
}

size_t chain_length(const swapchain & sc)
{
	return sc.images.size();
}

struct texture_chain
{
	std::unique_ptr<swapchain> images;
	ovrTextureSwapChainDesc desc{};
	bool srgb = false;
};

// frames composited without CPU waits (compositor::pipelined) reach the streamer from here, once their GPU work is
// done - the app's thread is already rendering the next frame meanwhile
class frame_submitter
{
public:
	struct job
	{
		compositor * comp;
		uint32_t generation, slot;
		vr_render_info info;
		int64_t display_time_ns;
	};
	~frame_submitter()
	{
		{
			std::lock_guard lock(m);
			stop = true;
		}
		cv.notify_all();
		if (thread.joinable())
			thread.join();
	}
	void push(vr_client * client, const job & j)
	{
		{
			std::lock_guard lock(m);
			if (!thread.joinable())
				thread = std::thread([this, client] { run(client); });
			queue.push_back(j);
			++queued;
		}
		cv.notify_all();
	}
	void drain() // before the compositor goes away
	{
		std::unique_lock lock(m);
		cv.wait(lock, [&] { return done == queued || stop; });
	}

private:
	void run(vr_client * client)
	{
		for (;;)
		{
			job j;
			{
				std::unique_lock lock(m);
				cv.wait(lock, [&] { return stop || !queue.empty(); });
				if (stop)
					return;
				j = queue.front();
				queue.pop_front();
			}
			j.comp->wait_composited(j.slot);
			client->send_submit(j.generation, j.slot, j.info, j.display_time_ns);
			{
				std::lock_guard lock(m);
				++done;
			}
			cv.notify_all();
		}
	}
	std::mutex m;
	std::condition_variable cv;
	std::deque<job> queue;
	uint64_t queued = 0, done = 0;
	bool stop = false;
	std::thread thread;
};

struct session_state
{
	std::unique_ptr<vr_client> client;
	std::mutex mutex;
	ipc::shared_state state{};
	double state_time = 0;

	ovrTrackingOrigin origin = ovrTrackingOrigin_EyeLevel;
	// recenter: tracking space = yaw(-center_yaw) * (raw - center)
	float center_yaw = 0;
	math::vec3 center{};
	bool centered = false;
	bool should_recenter = false;
	uint32_t recenter_serial = 0;
	double recenter_at = 0; // when to apply the headset's recenter (0: none pending)

	double period = 1.0 / 72;
	double next_frame = 0;
	int32_t begin_pose_index = 0;

	VkInstance vk_instance = VK_NULL_HANDLE;
	VkPhysicalDevice vk_physical = VK_NULL_HANDLE;       // last ovr_GetSessionPhysicalDeviceVk answer
	VkPhysicalDevice vk_bound_physical = VK_NULL_HANDLE; // the app's session binding (ovrvd_SetSessionPhysicalDeviceVk)
	bool announced_rendering = false;
	int render_tid = 0; // last thread reported as the one submitting frames
	int last_status_bits = -1;
	VkDevice vk_device = VK_NULL_HANDLE;
	VkQueue vk_queue = VK_NULL_HANDLE;
	uint32_t vk_family = 0;
	std::unique_ptr<compositor> comp;
	frame_submitter submitter; // after comp and client: stops before they go away
	float vibration[2] = {0, 0};
	uint64_t dropped = 0; // frames dropped for want of a free stream slot

	void refresh()
	{
		if (!client)
			return;
		client->poll(0);
		client->read_state(state);
		state_time = now_seconds();
		if (recenter_serial != state.settings.recenter_serial) {
			recenter_serial = state.settings.recenter_serial;
			recenter_at = state_time + 0.1; // the headset's own recenter: its next poses are in the moved space
		}
		if (state.frame_period_ns > 0)
			period = double(state.frame_period_ns) * 1e-9;
		if (!centered && state.frame_data_time_ns)
			recenter(false);
		// The headset's recenter moved its tracking origin, so the offset kept from the last centring is stale: without
		// this the player ended up that far away from the game's centre. Then tell the app its spaces moved.
		if (recenter_at > 0 && state_time >= recenter_at)
		{
			recenter_at = 0;
			if (recenter(false))
				should_recenter = true;
		}
	}

	frame_data::pose raw_head() const { return frame_data::head(state.frame_data); }

	// The app's forward = where the head looks now. Only from a real head pose: before the headset tracks (or while
	// it sends an all-zero pose) the forward vector is (0, 0, 0), and atan2(-0, -0) is -180 degrees - games then
	// started facing backwards. Until a valid pose arrives the raw space is used and the first one centres.
	bool recenter(bool keep_height)
	{
		frame_data::pose h = raw_head();
		const auto & q = h.orientation;
		const float norm = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
		if (norm < 0.5f || norm > 1.5f) // no pose yet (all zero)
			return false;
		math::vec3 fwd = math::rotate(math::normalize(h.orientation), {0, 0, -1});
		if (fwd[0] * fwd[0] + fwd[2] * fwd[2] > 1e-4f) // looking straight up or down: keep the previous yaw
			center_yaw = std::atan2(-fwd[0], -fwd[2]);
		float height = keep_height ? center[1] : h.position[1];
		center = {h.position[0], height, h.position[2]};
		centered = true;
		report("recentered: forward = headset yaw %.0f degrees, position (%.2f, %.2f)", double(center_yaw) * 180.0 / 3.14159265358979, double(center[0]),
		       double(center[2]));
		return true;
	}

	// raw headset tracking space -> app tracking space (origin type + recenter)
	void to_app(math::quat & q, math::vec3 & p) const
	{
		math::quat inv = math::yaw(-center_yaw);
		math::vec3 c = center;
		if (origin == ovrTrackingOrigin_FloorLevel)
			c[1] = -state.floor_height; // FrameData.FloorHeight: eye-level origin above the floor (0 with stage tracking)
		p = math::rotate(inv, math::sub(p, c));
		q = math::normalize(math::mul(inv, q));
	}

	void to_raw(math::quat & q, math::vec3 & p) const
	{
		math::quat fwd = math::yaw(center_yaw);
		math::vec3 c = center;
		if (origin == ovrTrackingOrigin_FloorLevel)
			c[1] = -state.floor_height;
		p = math::add(math::rotate(fwd, p), c);
		q = math::normalize(math::mul(fwd, q));
	}

	ovrPoseStatef pose_state(const frame_data::pose & raw, double abs_time) const
	{
		ovrPoseStatef s{};
		// predict from the FrameData arrival to the requested time (bounded: bad prediction is worse than none)
		double arrival = double(state.frame_data_time_ns) * 1e-9;
		float dt = abs_time > 0 && arrival > 0 ? float(std::clamp(abs_time - arrival, 0.0, 0.1)) : 0.f;
		math::quat q = math::integrate(raw.orientation, raw.angular_velocity, dt);
		math::vec3 p = math::add(raw.position, math::scale(raw.velocity, dt));
		to_app(q, p);
		math::quat inv = math::yaw(-center_yaw);
		math::vec3 w = math::rotate(inv, raw.angular_velocity), v = math::rotate(inv, raw.velocity);
		s.ThePose.Orientation = {q[0], q[1], q[2], q[3]};
		s.ThePose.Position = {p[0], p[1], p[2]};
		s.AngularVelocity = {w[0], w[1], w[2]};
		s.LinearVelocity = {v[0], v[1], v[2]};
		s.TimeInSeconds = abs_time > 0 ? abs_time : arrival;
		return s;
	}

	void eye_fov(int eye, ovrFovPort & f, bool cropped = true) const
	{
		const fov_port & p = state.fov[eye];
		f = {p.up > 0 ? p.up : 1.f, p.down > 0 ? p.down : 1.f, p.left > 0 ? p.left : 1.f, p.right > 0 ? p.right : 1.f};
		if (cropped) {
			f.UpTan *= state.settings.vertical_fov; f.DownTan *= state.settings.vertical_fov;
			f.LeftTan *= state.settings.horizontal_fov; f.RightTan *= state.settings.horizontal_fov;
		}
	}
};

session_state * S(ovrSession s)
{
	return reinterpret_cast<session_state *>(s);
}

int hand_of(ovrControllerType t)
{
	return t == ovrControllerType_RTouch ? 1 : 0;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// initialization

OVR_PUBLIC_FUNCTION(ovrDetectResult) ovr_Detect(int)
{
	ovrDetectResult r{};
	r.IsOculusServiceRunning = ipc::service_available();
	r.IsOculusHMDConnected = r.IsOculusServiceRunning;
	return r;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_Initialize(const ovrInitParams *)
{
	std::lock_guard lock(global_mutex);
	initialized = true;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_ReInitialize(const ovrInitParams *)
{
	return ovrSuccess; // visible/invisible mode switches need nothing here
}

OVR_PUBLIC_FUNCTION(void) ovr_Shutdown()
{
	std::lock_guard lock(global_mutex);
	initialized = false;
}

OVR_PUBLIC_FUNCTION(void) ovr_GetLastErrorInfo(ovrErrorInfo * info)
{
	if (!info)
		return;
	info->Result = last_error.empty() ? ovrSuccess : ovrError_RuntimeException;
	std::snprintf(info->ErrorString, sizeof(info->ErrorString), "%s", last_error.c_str());
}

OVR_PUBLIC_FUNCTION(const char *) ovr_GetVersionString()
{
	return "1.64.0-libovr-opendesk";
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_Create(ovrSession * out, ovrGraphicsLuid * luid)
{
	if (!out)
		return ovrError_InvalidParameter;
	std::string error;
	auto s = std::make_unique<session_state>();
	char exe[260] = {};
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	std::string app = n > 0 ? std::string(exe, size_t(n)) : "OpenXR app";
	app = app.substr(app.rfind('/') + 1);
	s->client = vr_client::connect(app, &error);
	if (!s->client)
		return fail(ovrError_ServiceConnection, error);
	s->refresh();
	if (luid)
	{
		std::memset(luid, 0, sizeof(*luid));
	}
	od::ovr::set_report_client(s->client.get());
	*out = reinterpret_cast<ovrSession>(s.release());
	log("session created for %s", app.c_str());
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(void) ovr_Destroy(ovrSession session)
{
	if (auto * s = S(session))
	{
		std::lock_guard lock(od::ovr::report_mutex);
		if (od::ovr::report_client == s->client.get())
			od::ovr::report_client = nullptr;
	}
	delete S(session);
}

// libovr-opendesk extension: a runtime (OpenDesk XR) message for the streamer's log
OVRVD_EXPORT void ovrvd_Log(const char * text)
{
	if (text)
		od::ovr::report_line(text);
}

// ---------------------------------------------------------------------------------------------------------------------
// device description

OVR_PUBLIC_FUNCTION(ovrHmdDesc) ovr_GetHmdDesc(ovrSession session)
{
	ovrHmdDesc d{};
	if (!session)
	{
		d.Type = ovrHmd_None;
		return d;
	}
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->refresh();
	d.Type = ovrHmdType(23); // ovrHmd_Quest3 (OpenDesk XR's OVR_Ext.h); OpenDesk XR only needs a Quest-class type
	// the model (OpenDesk XR picks controller pose conventions by it): MobileSettings.HmdType
	const char * product = "Meta Quest";
	switch (s->state.hmd_type)
	{
		case 320: product = "Meta Quest 2"; break;
		case 400: product = "Meta Quest Pro"; break;
		case 1000: product = "Meta Quest 3"; break;
		case 1001: product = "Meta Quest 3S"; break;
	}
	std::snprintf(d.ProductName, sizeof(d.ProductName), "%s", product);
	std::snprintf(d.Manufacturer, sizeof(d.Manufacturer), "Meta (OpenDesk, Linux)");
	d.VendorId = 0x2833;
	d.ProductId = 0x0200;
	std::snprintf(d.SerialNumber, sizeof(d.SerialNumber), "VDLINUX0001");
	d.AvailableHmdCaps = d.DefaultHmdCaps = 0;
	d.AvailableTrackingCaps = d.DefaultTrackingCaps = ovrTrackingCap_Orientation | ovrTrackingCap_MagYawCorrection | ovrTrackingCap_Position;
	for (int e = 0; e < 2; ++e)
	{
		s->eye_fov(e, d.DefaultEyeFov[e]);
		d.MaxEyeFov[e] = d.DefaultEyeFov[e];
	}
	const auto & c = s->client->config();
	d.Resolution = {int(c.eye_width * 2), int(c.eye_height)};
	d.DisplayRefreshRate = float(1.0 / s->period);
	return d;
}

OVR_PUBLIC_FUNCTION(ovrSizei) ovr_GetFovTextureSize(ovrSession session, ovrEyeType eye, ovrFovPort fov, float pixelsPerDisplayPixel)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	ovrFovPort def;
	s->eye_fov(eye, def, false);
	const auto & c = s->client->config();
	// the streamer's eye size is for the default FOV; scale with the tangent extents asked for
	float sx = (fov.LeftTan + fov.RightTan) / (def.LeftTan + def.RightTan);
	float sy = (fov.UpTan + fov.DownTan) / (def.UpTan + def.DownTan);
	pixelsPerDisplayPixel *= s->state.settings.resolution_scale;
	return {int(float(c.eye_width) * sx * pixelsPerDisplayPixel + 0.5f), int(float(c.eye_height) * sy * pixelsPerDisplayPixel + 0.5f)};
}

OVR_PUBLIC_FUNCTION(ovrEyeRenderDesc) ovr_GetRenderDesc2(ovrSession session, ovrEyeType eye, ovrFovPort fov)
{
	auto * s = S(session);
	ovrEyeRenderDesc d{};
	d.Eye = eye;
	d.Fov = fov;
	ovrSizei size = ovr_GetFovTextureSize(session, eye, fov, 1.f);
	d.DistortedViewport = {{0, 0}, size};
	d.PixelsPerTanAngleAtCenter = {float(size.w) / (fov.LeftTan + fov.RightTan), float(size.h) / (fov.UpTan + fov.DownTan)};
	std::lock_guard lock(s->mutex);
	float half = (s->state.ipd > 0 ? s->state.ipd : 0.063f) / 2;
	d.HmdToEyePose.Orientation = {0, 0, 0, 1};
	d.HmdToEyePose.Position = {eye == ovrEye_Left ? -half : half, 0, 0};
	return d;
}

// older name, same function (the header maps one to the other only on some platforms)
OVR_PUBLIC_FUNCTION(ovrEyeRenderDesc) ovr_GetRenderDesc(ovrSession session, ovrEyeType eye, ovrFovPort fov)
{
	return ovr_GetRenderDesc2(session, eye, fov);
}

// "Use Fov Stencil": what the lenses show of the rectangular eye image is round - an ellipse in viewing angle inscribed
// in the headset's full (asymmetric) FOV, grown by fov_stencil_margin so only what the lenses can't show is cut (about
// 9% of the image, like stencils made for these headsets). Games skip rendering outside it (OpenDesk XR:
// XR_KHR_visibility_mask). The ellipse belongs to the headset, not the game's FOV: with the FOV settings cropping the
// image its corners are visible, so less (or nothing) is hidden. Coordinates in [0,1], y down
// (ovrFovStencilFlag_MeshOriginAtBottomLeft: y up).
OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetFovStencil(ovrSession session, const ovrFovStencilDesc * desc, ovrFovStencilMeshBuffer * mesh)
{
	if (!desc || !mesh)
		return ovrError_InvalidParameter;
	auto * s = S(session);
	bool use = true;
	ovrFovPort lens{1, 1, 1, 1};
	{
		std::lock_guard lock(s->mutex);
		s->refresh();
		use = s->state.settings.use_stencil != 0;
		s->eye_fov(desc->Eye == ovrEye_Right ? 1 : 0, lens, false); // the headset's whole FOV
	}
	const ovrFovPort & f = desc->FovPort; // the image the mesh is for
	const float L = f.LeftTan, R = f.RightTan, U = f.UpTan, D = f.DownTan;
	std::vector<ovrVector2f> v;
	std::vector<uint16_t> idx;
	const bool flip = desc->StencilFlags & ovrFovStencilFlag_MeshOriginAtBottomLeft;
	auto uv = [&](float tx, float ty) { // tangents (x right, y up) -> image
		float y = (U - ty) / (U + D);
		return ovrVector2f{(tx + L) / (L + R), flip ? 1.f - y : y};
	};
	constexpr float margin = od::ovr::fov_stencil_margin;
	constexpr int n = 64; // rays
	constexpr float pi = 3.14159265358979f;
	auto visible = [&](float tx, float ty) { // inside the lens ellipse (viewing angles, per side)
		const float ax = std::atan(tx) / (std::atan(tx >= 0 ? lens.RightTan : lens.LeftTan) * margin);
		const float ay = std::atan(ty) / (std::atan(ty >= 0 ? lens.UpTan : lens.DownTan) * margin);
		return ax * ax + ay * ay <= 1.f;
	};
	auto rect = [&](float phi) { // where the ray at angle phi (tangent space) leaves the image
		float c = std::cos(phi), sn = std::sin(phi);
		float kx = c > 1e-6f ? R / c : c < -1e-6f ? -L / c : 1e9f, ky = sn > 1e-6f ? U / sn : sn < -1e-6f ? -D / sn : 1e9f;
		float k = std::min(kx, ky);
		return std::pair{c * k, sn * k};
	};
	auto edge = [&](float phi) { // where the ray leaves what is visible: the ellipse, or the image if that comes first
		auto [ox, oy] = rect(phi);
		if (visible(ox, oy))
			return std::pair{ox, oy};
		float lo = 0, hi = 1; // fraction of the way to the image border
		for (int i = 0; i < 24; ++i)
		{
			const float mid = (lo + hi) / 2;
			(visible(ox * mid, oy * mid) ? lo : hi) = mid;
		}
		return std::pair{ox * lo, oy * lo};
	};
	// ray angles: evenly spaced plus the exact image corners, so the outer ring follows the rectangle
	std::vector<float> phis;
	for (int i = 0; i < n; ++i)
		phis.push_back(2 * pi * float(i) / n);
	for (float corner: {std::atan2(U, R), std::atan2(U, -L), std::atan2(-D, -L), std::atan2(-D, R)})
		phis.push_back(corner < 0 ? corner + 2 * pi : corner);
	std::sort(phis.begin(), phis.end());
	const int m = int(phis.size());
	if (!use)
		; // nothing hidden: empty masks (games render the whole image)
	else if (desc->StencilType == ovrFovStencil_HiddenArea)
	{
		for (float phi: phis) // inner ring (the visible edge) and outer ring (the image border) on the same rays
		{
			auto [ix, iy] = edge(phi);
			auto [ox, oy] = rect(phi);
			v.push_back(uv(ix, iy));
			v.push_back(uv(ox, oy));
		}
		for (int i = 0; i < m; ++i)
		{
			uint16_t a = uint16_t(2 * i), b = uint16_t(2 * i + 1), c = uint16_t(2 * ((i + 1) % m)), d = uint16_t(2 * ((i + 1) % m) + 1);
			auto area = [&](uint16_t p, uint16_t q, uint16_t r) {
				return std::fabs((v[q].x - v[p].x) * (v[r].y - v[p].y) - (v[q].y - v[p].y) * (v[r].x - v[p].x));
			};
			for (auto [p, q, r]: {std::tuple{a, b, d}, std::tuple{a, d, c}}) // where the ellipse is outside the image: nothing hidden
				if (area(p, q, r) > 1e-7f)
					idx.insert(idx.end(), {p, q, r});
		}
		if (idx.empty())
			v.clear(); // the image lies inside the lens outline (FOV cropped): no mask
	}
	else if (desc->StencilType == ovrFovStencil_VisibleArea)
	{
		v.push_back(uv(0, 0));
		for (float phi: phis)
		{
			auto [x, y] = edge(phi);
			v.push_back(uv(x, y));
		}
		for (int i = 0; i < m; ++i)
			idx.insert(idx.end(), {0, uint16_t(1 + i), uint16_t(1 + (i + 1) % m)});
	}
	else if (desc->StencilType == ovrFovStencil_BorderLine)
	{
		for (float phi: phis)
		{
			auto [x, y] = edge(phi);
			v.push_back(uv(x, y));
		}
		for (int i = 0; i < m; ++i) // line list
			idx.insert(idx.end(), {uint16_t(i), uint16_t((i + 1) % m)});
	}
	else if (desc->StencilType == ovrFovStencil_VisibleRectangle)
	{
		// the largest axis-aligned rectangle inside the visible area along the diagonals (TopLeft, TopRight, BottomRight, BottomLeft)
		auto [lx, ty] = edge(std::atan2(U, -L));
		auto [rx, ty2] = edge(std::atan2(U, R));
		auto [lx2, by] = edge(std::atan2(-D, -L));
		auto [rx2, by2] = edge(std::atan2(-D, R));
		float left = std::max(lx, lx2), right = std::min(rx, rx2), top = std::min(ty, ty2), bottom = std::max(by, by2);
		v = {uv(left, top), uv(right, top), uv(right, bottom), uv(left, bottom)};
	}
	mesh->UsedVertexCount = int(v.size());
	mesh->UsedIndexCount = int(idx.size());
	if (mesh->AllocVertexCount == 0 && mesh->AllocIndexCount == 0)
		return ovrSuccess; // size query
	// (OpenDesk XR asks for line loops with half the index count it allocates - upstream's runtime takes that: only vertices checked)
	if (mesh->AllocVertexCount < mesh->UsedVertexCount)
		return ovrError_InvalidParameter;
	if (mesh->VertexBuffer)
		std::copy(v.begin(), v.end(), mesh->VertexBuffer);
	if (mesh->IndexBuffer)
		std::copy(idx.begin(), idx.end(), mesh->IndexBuffer);
	return ovrSuccess;
}

// ---------------------------------------------------------------------------------------------------------------------
// tracking and input

// OVR_CAPI_Util: eye poses from the head pose (eye = head * hmdToEye)
OVR_PUBLIC_FUNCTION(void) ovr_CalcEyePoses2(ovrPosef headPose, const ovrPosef HmdToEyePose[2], ovrPosef outEyePoses[2])
{
	math::quat hq{headPose.Orientation.x, headPose.Orientation.y, headPose.Orientation.z, headPose.Orientation.w};
	math::vec3 hp{headPose.Position.x, headPose.Position.y, headPose.Position.z};
	for (int e = 0; e < 2; ++e)
	{
		const ovrPosef & o = HmdToEyePose[e];
		math::quat q = math::mul(hq, {o.Orientation.x, o.Orientation.y, o.Orientation.z, o.Orientation.w});
		math::vec3 p = math::add(hp, math::rotate(hq, {o.Position.x, o.Position.y, o.Position.z}));
		outEyePoses[e].Orientation = {q[0], q[1], q[2], q[3]};
		outEyePoses[e].Position = {p[0], p[1], p[2]};
	}
}

OVR_PUBLIC_FUNCTION(double) ovr_GetTimeInSeconds()
{
	return now_seconds();
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SetTrackingOriginType(ovrSession session, ovrTrackingOrigin origin)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->origin = origin;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrTrackingOrigin) ovr_GetTrackingOriginType(ovrSession session)
{
	return S(session)->origin;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_RecenterTrackingOrigin(ovrSession session)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->refresh();
	s->recenter(false);
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(void) ovr_ClearShouldRecenterFlag(ovrSession session)
{
	std::lock_guard lock(S(session)->mutex);
	S(session)->should_recenter = false;
}

OVR_PUBLIC_FUNCTION(ovrTrackingState) ovr_GetTrackingState(ovrSession session, double absTime, ovrBool)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->refresh();
	ovrTrackingState t{};
	frame_data::pose head = s->raw_head();
	t.HeadPose = s->pose_state(head, absTime);
	t.StatusFlags = s->state.frame_data_time_ns ? (ovrStatus_OrientationTracked | ovrStatus_PositionTracked | ovrStatus_OrientationValid |
	                                               ovrStatus_PositionValid)
	                                            : 0;
	for (int h = 0; h < 2; ++h)
	{
		frame_data::pose c = frame_data::controller(s->state.frame_data, h);
		t.HandPoses[h] = s->pose_state(c, absTime);
		t.HandStatusFlags[h] = c.tracked ? (ovrStatus_OrientationTracked | ovrStatus_PositionTracked | ovrStatus_OrientationValid |
		                                    ovrStatus_PositionValid)
		                                 : 0;
	}
	t.CalibratedOrigin.Orientation = {0, 0, 0, 1};
	return t;
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetDevicePoses(ovrSession session, ovrTrackedDeviceType * types, int count, double absTime, ovrPoseStatef * out)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->refresh();
	for (int i = 0; i < count; ++i)
	{
		out[i] = {};
		out[i].ThePose.Orientation = {0, 0, 0, 1};
		if (types[i] == ovrTrackedDevice_HMD)
			out[i] = s->pose_state(s->raw_head(), absTime);
		else if (types[i] == ovrTrackedDevice_LTouch || types[i] == ovrTrackedDevice_RTouch)
			out[i] = s->pose_state(frame_data::controller(s->state.frame_data, types[i] == ovrTrackedDevice_RTouch), absTime);
	}
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(unsigned int) ovr_GetConnectedControllerTypes(ovrSession session)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->refresh();
	unsigned int types = 0;
	if (frame_data::controller(s->state.frame_data, 0).tracked)
		types |= ovrControllerType_LTouch;
	if (frame_data::controller(s->state.frame_data, 1).tracked)
		types |= ovrControllerType_RTouch;
	return types ? types : ovrControllerType_Touch; // report controllers even before they are seen
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetInputState(ovrSession session, ovrControllerType type, ovrInputState * in)
{
	if (!in)
		return ovrError_InvalidParameter;
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->refresh();
	*in = {};
	in->TimeInSeconds = now_seconds();
	if (!(type & ovrControllerType_Touch))
		return ovrSuccess;
	const uint8_t * fd = s->state.frame_data;
	uint16_t b = frame_data::buttons(fd), t = frame_data::touches(fd);
	using namespace frame_data;
	auto map = [](uint16_t v, std::initializer_list<std::pair<uint16_t, unsigned>> m) {
		unsigned r = 0;
		for (auto [from, to]: m)
			if (v & from)
				r |= to;
		return r;
	};
	in->Buttons = map(b, {{button_a, ovrButton_A}, {button_b, ovrButton_B}, {button_x, ovrButton_X}, {button_y, ovrButton_Y},
	                      {button_rthumb, ovrButton_RThumb}, {button_lthumb, ovrButton_LThumb}, {button_enter, ovrButton_Enter},
	                      {button_back, ovrButton_Back}});
	in->Touches = map(t, {{touch_a, ovrTouch_A}, {touch_b, ovrTouch_B}, {touch_x, ovrTouch_X}, {touch_y, ovrTouch_Y},
	                      {touch_rthumb, ovrTouch_RThumb}, {touch_lthumb, ovrTouch_LThumb}, {touch_rtrigger, ovrTouch_RIndexTrigger},
	                      {touch_ltrigger, ovrTouch_LIndexTrigger}, {touch_rthumbrest, ovrTouch_RThumbRest},
	                      {touch_lthumbrest, ovrTouch_LThumbRest}, {touch_rindex_pointing, ovrTouch_RIndexPointing},
	                      {touch_lindex_pointing, ovrTouch_LIndexPointing}, {touch_rthumb_up, ovrTouch_RThumbUp},
	                      {touch_lthumb_up, ovrTouch_LThumbUp}});
	for (int h = 0; h < 2; ++h)
	{
		if (type != ovrControllerType_Touch && hand_of(type) != h)
			continue;
		in->IndexTrigger[h] = in->IndexTriggerNoDeadzone[h] = in->IndexTriggerRaw[h] = frame_data::trigger(fd, h);
		in->HandTrigger[h] = in->HandTriggerNoDeadzone[h] = in->HandTriggerRaw[h] = frame_data::grip(fd, h);
		auto stick = frame_data::thumbstick(fd, h);
		in->ThumbstickRaw[h] = in->ThumbstickNoDeadzone[h] = {stick[0], stick[1]};
		float mag = std::sqrt(stick[0] * stick[0] + stick[1] * stick[1]);
		float scale = mag < 0.1f ? 0.f : (mag - 0.1f) / (0.9f * mag);
		in->Thumbstick[h] = {stick[0] * scale, stick[1] * scale};
	}
	in->ControllerType = ovrControllerType(type == ovrControllerType_Touch ? ovrControllerType_Touch : type);
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SetControllerVibration(ovrSession session, ovrControllerType type, float frequency, float amplitude)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	for (int h = 0; h < 2; ++h)
		if (type == ovrControllerType_Touch || hand_of(type) == h)
			if (s->vibration[h] != amplitude)
			{
				s->vibration[h] = amplitude;
				s->client->haptics(h, amplitude, frequency, 0);
			}
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetSessionStatus(ovrSession session, ovrSessionStatus * st)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->refresh();
	*st = {};
	bool alive = s->client->poll(0);
	st->HmdPresent = alive && s->state.headset_connected;
	st->HmdMounted = st->HmdPresent && s->state.frame_data[frame_data::offset::head_mounted];
	st->IsVisible = st->HmdPresent && s->state.vr_active;
	st->HasInputFocus = st->IsVisible;
	st->DisplayLost = !alive;
	st->ShouldQuit = !alive;
	st->ShouldRecenter = s->should_recenter;
	// status changes are rare and decide whether apps render or quit: always worth a line in the app's log
	int bits = st->HmdPresent | st->HmdMounted << 1 | st->IsVisible << 2 | st->ShouldQuit << 3 | st->DisplayLost << 4;
	if (bits != s->last_status_bits)
	{
		report("status: present %d mounted %d visible %d should-quit %d display-lost %d", st->HmdPresent, st->HmdMounted, st->IsVisible,
		       st->ShouldQuit, st->DisplayLost);
		s->last_status_bits = bits;
	}
	return ovrSuccess;
}

// properties: nothing tunable yet, answer with the caller's default
OVR_PUBLIC_FUNCTION(ovrBool) ovr_GetBool(ovrSession session, const char * key, ovrBool defaultVal) {
	if (!session || !key) return defaultVal;
	auto * s = S(session); std::lock_guard lock(s->mutex); s->refresh();
	const auto & p = s->state.settings;
	if (!std::strcmp(key, "SupportsHandTracking")) return !!(p.features & 0x20);
	if (!std::strcmp(key, "SupportsFaceTracking")) return !!(p.features & 0x40);
	if (!std::strcmp(key, "SupportsEyeTracking")) return !!(p.features & 0x80);
	if (!std::strcmp(key, "SupportsBodyTracking")) return !!(p.features & 0x100);
	if (!std::strcmp(key, "SupportsFullBodyTracking")) return !!(p.features & 0x200);
	if (!std::strcmp(key, "EmulateTrackers")) return p.emulate_trackers;
	if (!std::strcmp(key, "EmulateIndexControllers")) return p.emulate_index;
	return defaultVal;
}
OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetBool(ovrSession, const char *, ovrBool) { return ovrFalse; }
OVR_PUBLIC_FUNCTION(float) ovr_GetFloat(ovrSession session, const char * key, float defaultVal)
{
	// like OpenDesk's runtime: the eye height is FrameData.FloorHeight (0 in stage tracking, where the head
	// pose is already floor-relative; OpenDesk XR then infers LOCAL's height from the head)
	if (session && key && !std::strcmp(key, OVR_KEY_EYE_HEIGHT))
	{
		auto * s = S(session);
		std::lock_guard lock(s->mutex);
		s->refresh();
		return s->state.floor_height;
	}
	return defaultVal;
}
OVR_PUBLIC_FUNCTION(ovrBool) ovr_SetFloat(ovrSession, const char *, float) { return ovrFalse; }

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetPerfStats(ovrSession session, ovrPerfStats * stats)
{
	if (!stats)
		return ovrError_InvalidParameter;
	*stats = {};
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	// synchronous spacewarp: reported like LibOVR's ASW, so OpenDesk XR predicts two headset frames per app frame
	stats->FrameStatsCount = 1;
	stats->FrameStats[0].AswIsActive = s->state.settings.spacewarp ? ovrTrue : ovrFalse;
	stats->AswIsAvailable = ovrTrue;
	return ovrSuccess;
}

// ---------------------------------------------------------------------------------------------------------------------
// frame timing

OVR_PUBLIC_FUNCTION(double) ovr_GetPredictedDisplayTime(ovrSession session, long long)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	double base = s->next_frame > 0 ? s->next_frame : now_seconds();
	return base + s->period; // shown about one frame after submission
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_WaitToBeginFrame(ovrSession session, long long)
{
	auto * s = S(session);
	double wake;
	{
		std::lock_guard lock(s->mutex);
		s->refresh();
		double now = now_seconds();
		// synchronous spacewarp: one app frame every other headset frame (the headset synthesizes the others)
		const double app_period = s->period * (s->state.settings.spacewarp ? 2 : 1);
		if (s->next_frame <= 0 || s->next_frame < now - app_period)
			s->next_frame = now;
		wake = s->next_frame;
		s->next_frame += app_period;
	}
	double wait = wake - now_seconds();
	if (wait > 0)
		std::this_thread::sleep_for(std::chrono::duration<double>(wait));
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_BeginFrame(ovrSession session, long long)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->refresh();
	s->begin_pose_index = frame_data::get<int32_t>(s->state.frame_data, frame_data::offset::frame_index);
	if (!s->announced_rendering && s->comp && s->client && s->client->configured())
	{
		// a frame loop with swapchains: the streamer switches the headset to VR (probing clients never get here).
		// Apps render nothing until visible, and visible needs VR mode - so this can't wait for a submitted frame.
		s->client->announce_rendering();
		s->announced_rendering = true;
		// the game's first frame: where the player stands now is the spawn point, not where they stood when the game
		// attached to the runtime (it may have loaded for minutes, or started from the desktop with the headset elsewhere)
		s->recenter(false);
	}
	return ovrSuccess;
}

// ---------------------------------------------------------------------------------------------------------------------
// Vulkan

namespace
{
ovrResult copy_list(const char * list, char * out, uint32_t * inout)
{
	uint32_t need = uint32_t(std::strlen(list) + 1);
	if (!out || *inout < need)
	{
		bool too_small = out != nullptr;
		*inout = need;
		return too_small ? ovrError_InsufficientArraySize : ovrSuccess;
	}
	std::memcpy(out, list, need);
	*inout = need;
	return ovrSuccess;
}
} // namespace

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetInstanceExtensionsVk(ovrGraphicsLuid, char * names, uint32_t * size)
{
	return copy_list("VK_KHR_external_memory_capabilities VK_KHR_get_physical_device_properties2", names, size);
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetDeviceExtensionsVk(ovrGraphicsLuid, char * names, uint32_t * size)
{
	return copy_list(required_device_extensions(), names, size);
}

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_GetSessionPhysicalDeviceVk(ovrSession session, ovrGraphicsLuid, VkInstance instance, VkPhysicalDevice * out)
{
	auto * s = S(session);
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());
	if (devices.empty())
		return fail(ovrError_MismatchedAdapters, "no Vulkan device");
	const auto & config = s->client->config();
	VkPhysicalDevice pick = VK_NULL_HANDLE;
	VkPhysicalDevice fallback = VK_NULL_HANDLE;
	int matches = 0;
	for (VkPhysicalDevice d: devices)
	{
		VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
		VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drm};
		vkGetPhysicalDeviceProperties2(d, &p);
		if (drm.hasRender && drm.renderMajor == config.render_major && drm.renderMinor == config.render_minor) { pick = d; break; }
		// Wine can hide the Linux-only DRM properties; only accept an unambiguous vendor/device match.
		if (p.properties.vendorID == config.gpu_vendor && p.properties.deviceID == config.gpu_device) { fallback = d; ++matches; }
	}
	if (!pick && matches == 1) pick = fallback;
	if (!pick) return fail(ovrError_MismatchedAdapters, "No unambiguous Vulkan device matches the streamer's encoding GPU");
	s->vk_instance = instance;
	s->vk_physical = pick;
	*out = pick;
	log("ovr_GetSessionPhysicalDeviceVk: instance %p -> %p (%zu devices)", (void *)instance, (void *)pick, devices.size());
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_SetSynchronizationQueueVk(ovrSession session, VkQueue queue)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->vk_queue = queue;
	return ovrSuccess;
}

// libovr-opendesk extension (declared in OpenDesk XR's OVR_Ext.h): LibOVR has no way to learn the queue's family
OVRVD_EXPORT ovrResult ovrvd_SetSynchronizationQueueFamilyVk(ovrSession session, uint32_t family)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->vk_family = family;
	return ovrSuccess;
}

// libovr-opendesk extension: the application's own physical device (its session binding). ovr_GetSessionPhysicalDeviceVk
// may have been asked on another (since destroyed) instance, or on an earlier session - e.g. RiftLift's bridge
// probes with headless sessions first.
OVRVD_EXPORT ovrResult ovrvd_SetSessionPhysicalDeviceVk(ovrSession session, VkInstance instance,
                                                                                         VkPhysicalDevice physical)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->vk_instance = instance;
	s->vk_bound_physical = physical;
	log("session physical device %p (instance %p)", (void *)physical, (void *)instance);
	return ovrSuccess;
}

// libovr-opendesk extension: the raw headset FrameData (hands, eyes, face, body for OpenDesk XR's BodyStateV2)
OVRVD_EXPORT ovrResult ovrvd_GetFrameData(ovrSession session, uint8_t * buffer, uint32_t size)
{
	auto * s = S(session);
	if (!s || !buffer)
		return ovrError_InvalidParameter;
	std::lock_guard lock(s->mutex);
	s->refresh();
	const size_t n = std::min<size_t>(size, sizeof(s->state.frame_data));
	std::memcpy(buffer, s->state.frame_data, n);
	// hands and body come in the headset's raw tracking space; upstream's BodyState has them in the app's space (recentering)
	// at eye level: with a floor-level origin OpenDesk XR adds the eye height to hand joints, aim poses and body joints itself.
	// (Raw, the hands sat around the raw origin, e.g. behind the user's head; floor-level, an eye height too high.)
	const float eye_level = s->origin == ovrTrackingOrigin_FloorLevel ? s->state.floor_height : 0.f;
	auto pose = [&](size_t o, bool velocities) {
		if (o + (velocities ? 56 : 28) > n)
			return;
		math::quat q;
		math::vec3 pos;
		std::memcpy(q.data(), buffer + o, 16);
		std::memcpy(pos.data(), buffer + o + 16, 12);
		if (q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0)
			return; // no data
		s->to_app(q, pos);
		pos[1] -= eye_level;
		std::memcpy(buffer + o, q.data(), 16);
		std::memcpy(buffer + o + 16, pos.data(), 12);
		if (velocities) // FingerJointState: radius, angular velocity, linear velocity - rotate only
		{
			const math::quat yaw = math::yaw(-s->center_yaw);
			for (size_t v: {o + 32, o + 44})
			{
				math::vec3 w;
				std::memcpy(w.data(), buffer + v, 12);
				w = math::rotate(yaw, w);
				std::memcpy(buffer + v, w.data(), 12);
			}
		}
	};
	constexpr size_t hand_joints[2] = {316, 1772}, joint_bytes = 56, joint_count = 26;
	constexpr size_t aim[2] = {3232, 3288}, body = 3352, body_joint_bytes = 40, body_joints = 1280 / 40;
	for (size_t h = 0; h < 2; ++h)
	{
		for (size_t j = 0; j < joint_count; ++j)
			pose(hand_joints[h] + j * joint_bytes, true);
		pose(aim[h] + 8, false); // HandTrackingAimState: u64 status, then the aim pose
	}
	for (size_t j = 0; j < body_joints; ++j)
		pose(body + j * body_joint_bytes + 8, false); // u64 location flags, then the pose
	return ovrSuccess;
}

// libovr-opendesk extension: the body tracking skeleton (SkeletonDataChanged: joints x {i32 joint, i32 parent, pose}); false
// while the headset sent none
OVRVD_EXPORT bool ovrvd_GetBodySkeleton(ovrSession session, uint8_t * joints, uint32_t size,
                                                                          uint32_t * joint_count, int32_t * changed_count)
{
	auto * s = S(session);
	if (!s || !joints || !joint_count || !changed_count)
		return false;
	std::lock_guard lock(s->mutex);
	s->refresh();
	if (!s->state.skeleton_joint_count)
		return false;
	*joint_count = std::min<uint32_t>(s->state.skeleton_joint_count, size / uint32_t(ipc::skeleton_joint_bytes));
	*changed_count = s->state.skeleton_changed_count;
	std::memcpy(joints, s->state.skeleton, *joint_count * ipc::skeleton_joint_bytes);
	return true;
}

namespace
{
compositor * ensure_compositor(session_state * s, VkDevice device)
{
	if (s->comp && s->vk_device == device)
		return s->comp.get();
	if (!s->vk_queue)
		vkGetDeviceQueue(device, s->vk_family, 0, &s->vk_queue);
	s->vk_device = device;
	// the binding's device: later graphics-device queries may come with a temporary instance the app destroys again
	// (RiftLift's bridge; its dangling handle aborted the game in vkGetPhysicalDeviceMemoryProperties)
	log("compositor: app device %p, physical %p (bound %p, last queried %p), queue family %u", (void *)device,
	    (void *)(s->vk_bound_physical ? s->vk_bound_physical : s->vk_physical), (void *)s->vk_bound_physical, (void *)s->vk_physical,
	    s->vk_family);
	s->submitter.drain(); // frames still referencing the old compositor
	s->comp = std::make_unique<compositor>(s->vk_bound_physical ? s->vk_bound_physical : s->vk_physical, device, s->vk_queue,
	                                       s->vk_family);
	report("compositor: %s", s->comp->pipelined() ? "GPU-side sync with the app's queue" : "CPU waits (no external semaphores)");
	log("compositor ready");
	return s->comp.get();
}
} // namespace

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_CreateTextureSwapChainVk(ovrSession session, VkDevice device, const ovrTextureSwapChainDesc * desc, ovrTextureSwapChain * out)
{
	if (!desc || !out)
		return ovrError_InvalidParameter;
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	try
	{
		compositor * comp = ensure_compositor(s, device);
		bool depth, srgb;
		VkFormat format = vk_format_for(desc->Format, depth, srgb);
		if (format == VK_FORMAT_UNDEFINED)
			return fail(ovrError_InvalidParameter, "unsupported texture format " + std::to_string(desc->Format));
		auto chain = std::make_unique<texture_chain>();
		chain->desc = *desc;
		chain->srgb = srgb;
		uint32_t count = desc->StaticImage ? 1 : 3;
		chain->images = comp->create_swapchain(format, uint32_t(desc->Width), uint32_t(desc->Height), uint32_t(desc->ArraySize),
		                                       uint32_t(desc->MipLevels), uint32_t(desc->SampleCount), count, depth, desc->Type == ovrTexture_Cube);
		*out = reinterpret_cast<ovrTextureSwapChain>(chain.release());
		return ovrSuccess;
	}
	catch (const std::exception & e)
	{
		return fail(ovrError_RuntimeException, e.what());
	}
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetTextureSwapChainBufferVk(ovrSession, ovrTextureSwapChain chain, int index, VkImage * out)
{
	auto * c = reinterpret_cast<texture_chain *>(chain);
	if (!c || index < 0 || size_t(index) >= c->images->images.size())
		return ovrError_InvalidParameter; // (D3D11 chains have no VkImages on the app's side)
	*out = c->images->images[size_t(index)];
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetTextureSwapChainLength(ovrSession, ovrTextureSwapChain chain, int * out)
{
	*out = int(chain_length(*reinterpret_cast<texture_chain *>(chain)->images));
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetTextureSwapChainCurrentIndex(ovrSession, ovrTextureSwapChain chain, int * out)
{
	*out = reinterpret_cast<texture_chain *>(chain)->images->current;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_GetTextureSwapChainDesc(ovrSession, ovrTextureSwapChain chain, ovrTextureSwapChainDesc * out)
{
	*out = reinterpret_cast<texture_chain *>(chain)->desc;
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(ovrResult) ovr_CommitTextureSwapChain(ovrSession, ovrTextureSwapChain chain)
{
	auto * c = reinterpret_cast<texture_chain *>(chain);
	swapchain & sc = *c->images;
	sc.committed = sc.current;
	sc.current = (sc.current + 1) % int(chain_length(sc));
	return ovrSuccess;
}

// libovr-opendesk extension: commit a given image (OpenDesk XR keeps its own OpenXR image index)
OVRVD_EXPORT ovrResult ovrvd_ExportTextureSwapChainMemory(ovrSession session, ovrTextureSwapChain chain, int index, int * fd, uint64_t * size)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	auto * c = reinterpret_cast<texture_chain *>(chain);
	if (!s->comp || !c || !fd || !size || index < 0 || size_t(index) >= c->images->memory.size() || c->images->depth)
		return ovrError_InvalidParameter;
	try
	{
		*fd = s->comp->export_memory(*c->images, size_t(index));
		*size = c->images->memory_size[size_t(index)];
		return ovrSuccess;
	}
	catch (const std::exception & e)
	{
		return fail(ovrError_RuntimeException, e.what());
	}
}

OVRVD_EXPORT ovrResult ovrvd_CommitTextureSwapChainIndex(ovrSession, ovrTextureSwapChain chain, int index)
{
	auto * c = reinterpret_cast<texture_chain *>(chain);
	if (!c || index < 0 || size_t(index) >= chain_length(*c->images))
		return ovrError_InvalidParameter;
	c->images->committed = index;
	c->images->current = (index + 1) % int(chain_length(*c->images));
	return ovrSuccess;
}

OVR_PUBLIC_FUNCTION(void) ovr_DestroyTextureSwapChain(ovrSession session, ovrTextureSwapChain chain)
{
	auto * c = reinterpret_cast<texture_chain *>(chain);
	if (!c)
		return;
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	if (s->comp)
		s->comp->destroy_swapchain(*c->images);
	delete c;
}

OVR_PUBLIC_FUNCTION(void) ovr_DestroyMirrorTexture(ovrSession, ovrMirrorTexture) {}

// ---------------------------------------------------------------------------------------------------------------------
// frame submission

OVR_PUBLIC_FUNCTION(ovrResult)
ovr_EndFrame(ovrSession session, long long, const ovrViewScaleDesc *, ovrLayerHeader const * const * layers, unsigned int count)
{
	auto * s = S(session);
	std::lock_guard lock(s->mutex);
	s->refresh();
	if (!s->comp || !s->client->configured())
		return ovrSuccess;
	if (pid_t tid = gettid(); tid != s->render_tid)
	{
		s->render_tid = tid;
		s->client->render_thread(tid);
	}
	if (!s->state.vr_active)
		return ovrSuccess_NotVisible;
	auto slot = s->client->acquire(100);
	if (!slot)
	{
		// streamer busy: drop this frame (worth a line when it keeps happening)
		if (s->dropped++ % 100 == 0)
			report("no free stream slot for 100 ms: %llu frame(s) dropped so far", (unsigned long long)s->dropped);
		return ovrSuccess;
	}

	std::vector<composite_layer> list;
	bool have_pose = false;
	vr_render_info info;
	info.pose_index = s->begin_pose_index;
	math::quat eye_q[2];
	math::vec3 eye_p[2];
	float stream_fov[2][4];
	for (int e = 0; e < 2; ++e)
	{
		ovrFovPort f;
		s->eye_fov(e, f, false);
		stream_fov[e][0] = f.LeftTan;
		stream_fov[e][1] = f.RightTan;
		stream_fov[e][2] = f.UpTan;
		stream_fov[e][3] = f.DownTan;
	}
	auto rect_of = [](const swapchain & sc, const ovrRecti & vp, float * out) {
		float w = float(sc.width), h = float(sc.height);
		if (vp.Size.w <= 0 || vp.Size.h <= 0)
		{
			out[0] = out[1] = 0;
			out[2] = out[3] = 1;
			return;
		}
		out[0] = float(vp.Pos.x) / w;
		out[1] = float(vp.Pos.y) / h;
		out[2] = float(vp.Pos.x + vp.Size.w) / w;
		out[3] = float(vp.Pos.y + vp.Size.h) / h;
	};
	for (unsigned i = 0; i < count; ++i)
	{
		const ovrLayerHeader * h = layers[i];
		if (!h)
			continue;
		composite_layer l{};
		l.origin_bottom_left = h->Flags & ovrLayerFlag_TextureOriginAtBottomLeft;
		l.eye_mask = 3;
		if (h->Flags & 0x100000) // ovrLayerFlag_DisableLeft (OpenDesk XR)
			l.eye_mask &= ~1u;
		if (h->Flags & 0x200000) // ovrLayerFlag_DisableRight
			l.eye_mask &= ~2u;
		// array slice: libovr-opendesk keeps it in Header.Reserved[0..1] (OpenDesk XR port), 0 otherwise
		l.array_index[0] = uint8_t(h->Reserved[0]);
		l.array_index[1] = uint8_t(h->Reserved[1]);
		// alpha mode in Header.Reserved[2] (OpenDesk XR port): bit 0 opaque, bit 1 unpremultiplied
		l.opaque = h->Reserved[2] & 1;
		l.premultiplied = !(h->Reserved[2] & 2);
		// layer alpha (XR_KHR_composition_layer_color_scale_bias, OpenDesk XR port): Reserved[3] = 1, float in [4..7]
		if (h->Reserved[3] == 1)
		{
			std::memcpy(&l.alpha, &h->Reserved[4], sizeof(l.alpha));
			if (!(l.alpha > 0.f)) // faded out (an OpenVR loading screen after loading): not drawn at all
				continue;
		}
		if (h->Type == ovrLayerType_EyeFov || h->Type == ovrLayerType_EyeFovDepth)
		{
			auto * p = reinterpret_cast<const ovrLayerEyeFov *>(h);
			l.type = composite_layer::projection;
			for (int e = 0; e < 2; ++e)
			{
				auto * c = reinterpret_cast<texture_chain *>(p->ColorTexture[e] ? p->ColorTexture[e] : p->ColorTexture[0]);
				if (!c)
					continue;
				l.chain[e] = c->images.get();
				rect_of(*c->images, p->Viewport[e], l.rect[e]);
				const ovrFovPort & f = p->Fov[e];
				l.fov[e][0] = f.LeftTan;
				l.fov[e][1] = f.RightTan;
				l.fov[e][2] = f.UpTan;
				l.fov[e][3] = f.DownTan;
				if (!have_pose)
				{
					eye_q[e] = {p->RenderPose[e].Orientation.x, p->RenderPose[e].Orientation.y, p->RenderPose[e].Orientation.z,
					            p->RenderPose[e].Orientation.w};
					eye_p[e] = {p->RenderPose[e].Position.x, p->RenderPose[e].Position.y, p->RenderPose[e].Position.z};
				}
			}
			have_pose = true;
		}
		else if (h->Type == ovrLayerType_Quad)
		{
			auto * q = reinterpret_cast<const ovrLayerQuad *>(h);
			auto * c = reinterpret_cast<texture_chain *>(q->ColorTexture);
			if (!c)
				continue;
			l.type = composite_layer::quad;
			l.chain[0] = l.chain[1] = c->images.get();
			rect_of(*c->images, q->Viewport, l.rect[0]);
			std::memcpy(l.rect[1], l.rect[0], sizeof(l.rect[0]));
			l.quad_orientation = {q->QuadPoseCenter.Orientation.x, q->QuadPoseCenter.Orientation.y, q->QuadPoseCenter.Orientation.z,
			                      q->QuadPoseCenter.Orientation.w};
			l.quad_position = {q->QuadPoseCenter.Position.x, q->QuadPoseCenter.Position.y, q->QuadPoseCenter.Position.z};
			l.quad_size[0] = q->QuadSize.x;
			l.quad_size[1] = q->QuadSize.y;
			l.head_locked = h->Flags & ovrLayerFlag_HeadLocked;
		}
		else if (h->Type == ovrLayerType_Cylinder) {
			auto * q = reinterpret_cast<const ovrLayerCylinder *>(h);
			auto * c = reinterpret_cast<texture_chain *>(q->ColorTexture);
			if (!c || q->CylinderRadius <= 0 || q->CylinderAngle <= 0 || q->CylinderAspectRatio <= 0) continue;
			l.type = composite_layer::cylinder; l.chain[0] = l.chain[1] = c->images.get();
			rect_of(*c->images, q->Viewport, l.rect[0]); std::memcpy(l.rect[1], l.rect[0], sizeof(l.rect[0]));
			l.quad_orientation = {q->CylinderPoseCenter.Orientation.x,q->CylinderPoseCenter.Orientation.y,q->CylinderPoseCenter.Orientation.z,q->CylinderPoseCenter.Orientation.w};
			l.quad_position = {q->CylinderPoseCenter.Position.x,q->CylinderPoseCenter.Position.y,q->CylinderPoseCenter.Position.z};
			l.cylinder_radius=q->CylinderRadius; l.cylinder_angle=q->CylinderAngle; l.cylinder_aspect=q->CylinderAspectRatio;
			l.head_locked = h->Flags & ovrLayerFlag_HeadLocked;
		} else if (h->Type == ovrLayerType_Cube) {
			auto * q = reinterpret_cast<const ovrLayerCube *>(h);
			auto * c = reinterpret_cast<texture_chain *>(q->CubeMapTexture);
			if (!c || c->images->layers < 6) continue;
			l.type = composite_layer::cube; l.chain[0] = l.chain[1] = c->images.get();
			l.quad_orientation = {q->Orientation.x,q->Orientation.y,q->Orientation.z,q->Orientation.w};
			l.head_locked = h->Flags & ovrLayerFlag_HeadLocked;
			l.rect[0][2]=l.rect[0][3]=l.rect[1][2]=l.rect[1][3]=1;
			for (int face=0; face<6; ++face) {
				l.cube_face=face; l.array_index[0]=l.array_index[1]=face; list.push_back(l);
			}
			continue;
		} else continue;
		list.push_back(l);
	}

	// frame pose: the projection layer's render pose, else the current head pose (quads only)
	ovrTrackingState ts{};
	frame_data::pose head = s->raw_head();
	ovrPoseStatef hs = s->pose_state(head, 0);
	math::quat head_q{hs.ThePose.Orientation.x, hs.ThePose.Orientation.y, hs.ThePose.Orientation.z, hs.ThePose.Orientation.w};
	math::vec3 head_p{hs.ThePose.Position.x, hs.ThePose.Position.y, hs.ThePose.Position.z};
	(void)ts;
	if (!have_pose)
	{
		float half = (s->state.ipd > 0 ? s->state.ipd : 0.063f) / 2;
		for (int e = 0; e < 2; ++e)
		{
			eye_q[e] = head_q;
			eye_p[e] = math::add(head_p, math::rotate(head_q, {e ? half : -half, 0, 0}));
		}
	}
	try
	{
		// foveated streaming: each eye's fovea crop centred on that eye's latest gaze exactly like the Windows streamer:
		// the headset's per-eye projection NDC (each through that eye's own asymmetric FOV, so the two differ for the
		// same direction), centre = (gaze / 2 + 0.5) * eye size, no smoothing, prediction or reprojection
		const auto & cfg = s->client->config();
		if (cfg.foveated)
			for (int e = 0; e < 2; ++e)
			{
				const int ew = int(cfg.eye_width), eh = int(cfg.eye_height);
				const int fw = int(cfg.fovea_image.width), fh = int(cfg.fovea_image.height);
				const float gx = s->state.gaze_valid ? s->state.gaze[e][0] : 0.f, gy = s->state.gaze_valid ? s->state.gaze[e][1] : 0.f;
				const int cx = int(std::lround((gx / 2 + 0.5f) * float(ew))), cy = int(std::lround((gy / 2 + 0.5f) * float(eh)));
				info.fovea_top_left[e][0] = std::clamp(cx - fw / 2, 0, std::max(ew - fw, 0));
				info.fovea_top_left[e][1] = std::clamp(cy - fh / 2, 0, std::max(eh - fh, 0));
			}
		double t0 = now_seconds();
		bool composited = s->comp->composite(*s->client, *slot, list, stream_fov, eye_q, eye_p, head_q, head_p,
		                                     cfg.foveated ? info.fovea_top_left : nullptr);
		if (double took = now_seconds() - t0; took > 0.5)
			report("compositing one frame took %.0f ms (GPU busy or stalled)", took * 1000);
		if (!composited)
			return ovrSuccess;
	}
	catch (const std::exception & e)
	{
		return fail(ovrError_RuntimeException, e.what());
	}
	// the headset reprojects from the render pose: give it in the headset's own tracking space
	for (int e = 0; e < 2; ++e)
	{
		math::quat q = eye_q[e];
		math::vec3 p = eye_p[e];
		s->to_raw(q, p);
		info.eye[e].orientation = q;
		info.eye[e].position = p;
		info.fov[e] = {stream_fov[e][2], stream_fov[e][3], stream_fov[e][0], stream_fov[e][1]};
	}
	const int64_t display_time = ipc::monotonic_ns();
	if (s->comp->pipelined())
	{
		s->client->reserve(*slot); // this thread polls the releases; the submitter only sends
		s->submitter.push(s->client.get(), {s->comp.get(), s->client->generation(), *slot, info, display_time});
	}
	else
		s->client->submit(*slot, info, display_time);
	return ovrSuccess;
}

