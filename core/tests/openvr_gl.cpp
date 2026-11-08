// OpenVR rendering with OpenGL (GLX), the path Vivecraft takes: xrizer turns it into an OpenXR OpenGL session on OpenDesk
// XR. Submits a 256x256 texture whose top half (OpenGL's y >= 128) is red and bottom half green.
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#undef None
#undef Bool
#include <openvr_capi.h>
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

using create_context_t = GLXContext (*)(Display *, GLXFBConfig, GLXContext, int, const int *);

int main(int argc, char ** argv)
try
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	const char * path = getenv("OPENDESK_OPENVR_API");
	if (!path)
		return 77;
	Display * display = XOpenDisplay(nullptr);
	if (!display)
		throw std::runtime_error("no X display");
	const int fb_attrs[] = {GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, 0L};
	int configs = 0;
	GLXFBConfig * fb = glXChooseFBConfig(display, DefaultScreen(display), fb_attrs, &configs);
	if (!fb || !configs)
		throw std::runtime_error("no GLX fbconfig");
	auto create = reinterpret_cast<create_context_t>(glXGetProcAddressARB(reinterpret_cast<const GLubyte *>("glXCreateContextAttribsARB")));
	const int ctx_attrs[] = {0x2091 /* MAJOR */, 4, 0x2092 /* MINOR */, 5, 0L};
	GLXContext context = create(display, fb[0], nullptr, 1, ctx_attrs);
	const int pb_attrs[] = {GLX_PBUFFER_WIDTH, 16, GLX_PBUFFER_HEIGHT, 16, 0L};
	GLXPbuffer pbuffer = glXCreatePbuffer(display, fb[0], pb_attrs);
	if (!context || !glXMakeContextCurrent(display, pbuffer, pbuffer, context))
		throw std::runtime_error("no GLX context");
	printf("OpenGL: %s\n", glGetString(GL_RENDERER));

	void * lib = dlopen(path, RTLD_NOW);
	if (!lib)
		throw std::runtime_error(dlerror());
	auto init = reinterpret_cast<uint32_t (*)(int *, int, const char *)>(dlsym(lib, "VR_InitInternal2"));
	auto get = reinterpret_cast<void * (*)(const char *, int *)>(dlsym(lib, "VR_GetGenericInterface"));
	auto shutdown = reinterpret_cast<void (*)()>(dlsym(lib, "VR_ShutdownInternal"));
	int error = 0;
	init(&error, 1, nullptr);
	if (error)
		throw std::runtime_error("VR_Init " + std::to_string(error));
	auto * compositor = static_cast<VR_IVRCompositor_FnTable *>(get("FnTable:IVRCompositor_027", &error));
	if (!compositor)
		throw std::runtime_error("IVRCompositor_027 unavailable");

	GLuint texture = 0;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	GLuint fbo = 0;
	auto gen_fbo = reinterpret_cast<void (*)(GLsizei, GLuint *)>(glXGetProcAddressARB(reinterpret_cast<const GLubyte *>("glGenFramebuffers")));
	auto bind_fbo = reinterpret_cast<void (*)(GLenum, GLuint)>(glXGetProcAddressARB(reinterpret_cast<const GLubyte *>("glBindFramebuffer")));
	auto attach = reinterpret_cast<void (*)(GLenum, GLenum, GLenum, GLuint, GLint)>(
	        glXGetProcAddressARB(reinterpret_cast<const GLubyte *>("glFramebufferTexture2D")));
	gen_fbo(1, &fbo);
	bind_fbo(0x8D40 /* GL_FRAMEBUFFER */, fbo);
	attach(0x8D40, 0x8CE0 /* COLOR_ATTACHMENT0 */, GL_TEXTURE_2D, texture, 0);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 128, 256, 128);
	glClearColor(0.8f, 0.1f, 0.1f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glScissor(0, 0, 256, 128);
	glClearColor(0.1f, 0.8f, 0.1f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	bind_fbo(0x8D40, 0);

	Texture_t vr_texture{reinterpret_cast<void *>(uintptr_t(texture)), ETextureType_TextureType_OpenGL, EColorSpace_ColorSpace_Linear};
	TrackedDevicePose_t poses[16]{};
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(argc > 1 ? atoi(argv[1]) : 8);
	int frames = 0;
	while (std::chrono::steady_clock::now() < end)
	{
		compositor->WaitGetPoses(poses, 16, nullptr, 0);
		for (int eye = 0; eye < 2; ++eye)
		{
			auto r = compositor->Submit(EVREye(eye), &vr_texture, nullptr, EVRSubmitFlags_Submit_Default);
			if (r != EVRCompositorError_VRCompositorError_None)
				throw std::runtime_error("Submit " + std::to_string(r));
		}
		compositor->PostPresentHandoff();
		++frames;
	}
	shutdown();
	printf("%s rendered %d stereo frames through xrizer with OpenGL\n", frames > 20 ? "ALL PASS" : "FAIL", frames);
	return frames > 20 ? 0 : 1;
}
catch (const std::exception & e)
{
	fprintf(stderr, "FAIL %s\n", e.what());
	return 1;
}
