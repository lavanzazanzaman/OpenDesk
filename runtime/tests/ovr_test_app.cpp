// Headless LibOVR app for libovr-opendesk: the same calls OpenDesk XR makes. Creates a Vulkan device with the extensions the runtime
// asks for, one texture swapchain per eye filled with a labelled grid (left eye green, right eye magenta), and a quad
// layer, then runs the Oculus frame loop (WaitToBeginFrame / BeginFrame / EndFrame).
//   ovr-test-app [SECONDS]
#include <vulkan/vulkan.h>
#include <OVR_CAPI.h>
#include <OVR_CAPI_Vk.h>

#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

extern "C" ovrResult ovrvd_SetSynchronizationQueueFamilyVk(ovrSession, uint32_t);
extern "C" ovrResult ovrvd_GetFrameData(ovrSession, uint8_t *, uint32_t);

static std::vector<std::string> split(const std::string & s)
{
	std::vector<std::string> out;
	std::istringstream in(s);
	for (std::string w; in >> w;)
		out.push_back(w);
	return out;
}

#define CHECK(x)                                                                                                                 \
	do                                                                                                                           \
	{                                                                                                                            \
		auto r_ = (x);                                                                                                           \
		if (int(r_) < 0)                                                                                                         \
		{                                                                                                                        \
			ovrErrorInfo e_{};                                                                                                   \
			ovr_GetLastErrorInfo(&e_);                                                                                           \
			std::fprintf(stderr, "%s failed: %d %s\n", #x, int(r_), e_.ErrorString);                                           \
			return 1;                                                                                                            \
		}                                                                                                                        \
	} while (0)

int main(int argc, char ** argv)
{
	int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    const int samples = getenv("OVR_TEST_MSAA") ? 4 : 1;
    const std::string mode = getenv("OVR_TEST_LAYER") ? getenv("OVR_TEST_LAYER") : "projection";
    const int faces = mode == "cube" ? 6 : 1;
	CHECK(ovr_Initialize(nullptr));
	ovrSession session;
	ovrGraphicsLuid luid;
	CHECK(ovr_Create(&session, &luid));
	if (getenv("OPENDESK_TEST_HANDS")) // like OpenDesk XR (floor-level origin, which then adds the eye height to hand/body joints)
		CHECK(ovr_SetTrackingOriginType(session, ovrTrackingOrigin_FloorLevel));
	ovrHmdDesc hmd = ovr_GetHmdDesc(session);
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	std::printf("HMD '%s' %dx%d @%.0f Hz, FOV L %.2f/%.2f/%.2f/%.2f\n", hmd.ProductName, hmd.Resolution.w, hmd.Resolution.h,
	            hmd.DisplayRefreshRate, hmd.DefaultEyeFov[0].LeftTan, hmd.DefaultEyeFov[0].RightTan, hmd.DefaultEyeFov[0].UpTan,
	            hmd.DefaultEyeFov[0].DownTan);

	// Vulkan instance + device with the runtime's extensions
	char buf[1024];
	uint32_t n = sizeof(buf);
	CHECK(ovr_GetInstanceExtensionsVk(luid, buf, &n));
	auto inst_ext = split(buf);
	std::vector<const char *> ie;
	for (auto & e: inst_ext)
		ie.push_back(e.c_str());
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = uint32_t(ie.size());
	ici.ppEnabledExtensionNames = ie.data();
	VkInstance instance;
	if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
		return std::fprintf(stderr, "vkCreateInstance failed\n"), 1;
	VkPhysicalDevice physical;
	CHECK(ovr_GetSessionPhysicalDeviceVk(session, luid, instance, &physical));
	uint32_t qcount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, nullptr);
	std::vector<VkQueueFamilyProperties> qf(qcount);
	vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, qf.data());
	uint32_t family = 0;
	for (uint32_t i = 0; i < qcount; ++i)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			family = i;
			break;
		}
	n = sizeof(buf);
	CHECK(ovr_GetDeviceExtensionsVk(luid, buf, &n));
	auto dev_ext = split(buf);
	std::vector<const char *> de;
	for (auto & e: dev_ext)
		de.push_back(e.c_str());
	float prio = 1;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = uint32_t(de.size());
	dci.ppEnabledExtensionNames = de.data();
	VkDevice device;
	if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS)
		return std::fprintf(stderr, "vkCreateDevice failed\n"), 1;
	VkQueue queue;
	vkGetDeviceQueue(device, family, 0, &queue);
	CHECK(ovrvd_SetSynchronizationQueueFamilyVk(session, family));
	CHECK(ovr_SetSynchronizationQueueVk(session, queue));

	// per-eye swapchains, filled once with the eye's grid (copied from a host buffer); OPENDESK_TEST_DETAIL=1: a detailed
	// pattern instead (upscaling comparisons)
	const bool detail = std::getenv("OPENDESK_TEST_DETAIL") != nullptr;
	ovrTextureSwapChain chain[2];
	ovrSizei size[2];
	VkCommandPool pool;
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.queueFamilyIndex = family;
	vkCreateCommandPool(device, &pci, nullptr, &pool);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(physical, &mp);
	auto host_type = [&](uint32_t bits) {
		for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
			if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			                                  (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
				return i;
		return 0u;
	};
	for (int e = 0; e < 2; ++e)
	{
		size[e] = ovr_GetFovTextureSize(session, ovrEyeType(e), hmd.DefaultEyeFov[e], 1.0f);
		ovrTextureSwapChainDesc d{};
		d.Type = faces == 6 ? ovrTexture_Cube : ovrTexture_2D;
		d.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
		d.ArraySize = faces;
		if (faces == 6) size[e].w = size[e].h = 256;
		d.Width = size[e].w;
		d.Height = size[e].h;
		d.MipLevels = 1;
		d.SampleCount = samples;
		CHECK(ovr_CreateTextureSwapChainVk(session, device, &d, &chain[e]));
		// grid: lines every 1/10, eye colour, a white centre cross
		VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bci.size = VkDeviceSize(size[e].w) * size[e].h * 4;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		VkBuffer buffer;
		vkCreateBuffer(device, &bci, nullptr, &buffer);
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device, buffer, &req);
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = host_type(req.memoryTypeBits);
		VkDeviceMemory mem;
		vkAllocateMemory(device, &mai, nullptr, &mem);
		vkBindBufferMemory(device, buffer, mem, 0);
		uint8_t * px;
		vkMapMemory(device, mem, 0, bci.size, 0, reinterpret_cast<void **>(&px));
		for (int y = 0; y < size[e].h; ++y)
			for (int x = 0; x < size[e].w; ++x)
			{
				uint8_t * p = px + (size_t(y) * size[e].w + x) * 4;
				bool line = (x * 10 / size[e].w) != ((x + 1) * 10 / size[e].w) || (y * 10 / size[e].h) != ((y + 1) * 10 / size[e].h) ||
				            x < 3 || y < 3 || x >= size[e].w - 3 || y >= size[e].h - 3;
				bool cross = (std::abs(x - size[e].w / 2) < 3 && std::abs(y - size[e].h / 2) < size[e].h / 8) ||
				             (std::abs(y - size[e].h / 2) < 3 && std::abs(x - size[e].w / 2) < size[e].w / 8);
				uint8_t r = e == 0 ? 20 : 150, g = e == 0 ? 120 : 20, b = e == 0 ? 40 : 150;
				if (line)
					r = g = b = 200;
				if (cross)
					r = g = b = 255;
				if (detail) // the same scene at any resolution, antialiased (4x4 samples): zone plate + thin diagonal lines
				{
					float sum = 0;
					for (int sy = 0; sy < 4; ++sy)
						for (int sx = 0; sx < 4; ++sx)
						{
							float u = (x + (sx + .5f) / 4) / size[e].w - .5f, v = (y + (sy + .5f) / 4) / size[e].h - .5f;
							float z = .5f + .5f * std::cos(2 * 3.14159265f * 400 * (u * u + v * v));
							float d = std::fmod(std::abs(u * 1.3f - v) * 24, 1.f); // diagonal lines, ~1/400 of the width
							sum += (u > 0 && d < .02f) ? 1.f : (u > 0 ? .15f : z);
						}
					r = g = b = uint8_t(std::lround(255 * std::pow(sum / 16, 1 / 2.2f)));
				}
				p[0] = r;
				p[1] = g;
				p[2] = b;
				p[3] = 255;
			}
		int len = 0;
		ovr_GetTextureSwapChainLength(session, chain[e], &len);
		for (int i = 0; i < len; ++i)
		{
			VkImage img;
			ovr_GetTextureSwapChainBufferVk(session, chain[e], i, &img);
			VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			cai.commandPool = pool;
			cai.commandBufferCount = 1;
			VkCommandBuffer cb;
			vkAllocateCommandBuffers(device, &cai, &cb);
			VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			vkBeginCommandBuffer(cb, &bi);
			VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = img;
			b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, uint32_t(faces)};
			b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			VkBufferImageCopy copy{};
			copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.imageExtent = {uint32_t(size[e].w), uint32_t(size[e].h), 1};
			if (samples == 1 && faces == 1) {
                vkCmdCopyBufferToImage(cb, buffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            } else {
                // Clears support multisample images and make resolve/face-selection measurable.
                for (int face = 0; face < faces; ++face) {
                    VkClearColorValue color{{face == 5 ? .1f : .7f, .3f, .2f, 1.f}};
                    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, uint32_t(face), 1};
                    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
                }
            }
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			vkEndCommandBuffer(cb);
			VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cb;
			vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
			vkQueueWaitIdle(queue);
		}
	}
	std::printf("swapchains %dx%d per eye, running %d s\n", size[0].w, size[0].h, seconds);
	std::fflush(stdout);

	ovrEyeRenderDesc render[2] = {ovr_GetRenderDesc(session, ovrEye_Left, hmd.DefaultEyeFov[0]),
	                              ovr_GetRenderDesc(session, ovrEye_Right, hmd.DefaultEyeFov[1])};
	auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	long long frame = 0;
	int visible_frames = 0;
	while (std::chrono::steady_clock::now() < end)
	{
		ovrSessionStatus st;
		ovr_GetSessionStatus(session, &st);
		if (st.ShouldQuit)
			break;
		CHECK(ovr_WaitToBeginFrame(session, frame));
		CHECK(ovr_BeginFrame(session, frame));
		double t = ovr_GetPredictedDisplayTime(session, frame);
		ovrTrackingState ts = ovr_GetTrackingState(session, t, ovrTrue);
		if (frame == 60 && getenv("OPENDESK_TEST_HANDS"))
		{
			// hand joints (OpenDesk XR's BodyState source) are at eye level, like upstream's BodyState: OpenDesk XR adds the eye height
			// (floor-level origin) and must land on the controllers' space
			static uint8_t fd[4992];
			ovrvd_GetFrameData(session, fd, sizeof(fd));
			float joint[3];
			std::memcpy(joint, fd + 316 + 16, 12);
			joint[1] += ovr_GetFloat(session, OVR_KEY_EYE_HEIGHT, 0.f);
			const ovrVector3f & c = ts.HandPoses[ovrHand_Left].ThePose.Position;
			float d = std::sqrt((joint[0] - c.x) * (joint[0] - c.x) + (joint[1] - c.y) * (joint[1] - c.y) + (joint[2] - c.z) * (joint[2] - c.z));
			std::printf("%s  hand joint in the controllers' space (joint %.3f %.3f %.3f, controller %.3f %.3f %.3f)\n", d < 0.01f ? "PASS" : "FAIL",
			            joint[0], joint[1], joint[2], c.x, c.y, c.z);
		}
		// eye = head * hmd_to_eye (translation along the head's axes)
		ovrPosef eye_pose[2];
		for (int e = 0; e < 2; ++e)
		{
			const ovrQuatf & q = ts.HeadPose.ThePose.Orientation;
			const ovrVector3f & o = render[e].HmdToEyePose.Position;
			float x = q.x, y = q.y, z = q.z, w = q.w;
			float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
			                 {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
			                 {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
			eye_pose[e].Orientation = q;
			eye_pose[e].Position = {ts.HeadPose.ThePose.Position.x + r[0][0] * o.x + r[0][1] * o.y + r[0][2] * o.z,
			                        ts.HeadPose.ThePose.Position.y + r[1][0] * o.x + r[1][1] * o.y + r[1][2] * o.z,
			                        ts.HeadPose.ThePose.Position.z + r[2][0] * o.x + r[2][1] * o.y + r[2][2] * o.z};
		}
		for (int e = 0; e < 2; ++e)
			CHECK(ovr_CommitTextureSwapChain(session, chain[e]));
		ovrLayerEyeFov layer{};
		layer.Header.Type = ovrLayerType_EyeFov;
		for (int e = 0; e < 2; ++e)
		{
			layer.ColorTexture[e] = chain[e];
			layer.Viewport[e] = {{0, 0}, size[e]};
			layer.Fov[e] = hmd.DefaultEyeFov[e];
			layer.RenderPose[e] = eye_pose[e];
		}
		layer.SensorSampleTime = t;
		ovrLayerCylinder cylinder{};
        cylinder.Header.Type = ovrLayerType_Cylinder;
        cylinder.Header.Flags = ovrLayerFlag_HeadLocked;
        cylinder.ColorTexture = chain[0]; cylinder.Viewport = {{0,0},size[0]};
        cylinder.CylinderPoseCenter.Orientation.w = 1;
        cylinder.CylinderRadius = 2; cylinder.CylinderAngle = 3.14f; cylinder.CylinderAspectRatio = 2;
        ovrLayerCube cube{}; cube.Header.Type = ovrLayerType_Cube;
        cube.Header.Flags = ovrLayerFlag_HeadLocked; cube.Orientation.w = 1; cube.CubeMapTexture = chain[0];
        const ovrLayerHeader * layers[] = {mode == "cylinder" ? &cylinder.Header : mode == "cube" ? &cube.Header : &layer.Header};
		ovrResult r = ovr_EndFrame(session, frame, nullptr, layers, 1);
		if (OVR_FAILURE(r)) { ovrErrorInfo error{}; ovr_GetLastErrorInfo(&error); fprintf(stderr,"FAIL EndFrame %d %s\n", r,error.ErrorString); return 1; }
		if (r == ovrSuccess)
			++visible_frames;
		if (++frame % 360 == 0)
		{
			std::printf("%lld frames (%d composited), visible %d, head y %.2f\n", frame, visible_frames, st.IsVisible,
			            ts.HeadPose.ThePose.Position.y);
			std::fflush(stdout);
		}
	}
	std::printf("%s: %lld frames, %d composited, %d samples, %s layer\n", visible_frames > 20 ? "ALL PASS" : "FAIL", frame, visible_frames, samples, mode.c_str());
	for (int e = 0; e < 2; ++e)
		ovr_DestroyTextureSwapChain(session, chain[e]);
	ovr_Destroy(session);
	ovr_Shutdown();
	return visible_frames > 20 ? 0 : 1;
}
