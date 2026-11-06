#include "test_pattern.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#include <stdexcept>
#include <string>

namespace od
{
namespace
{
const char * vertex_src = R"(#version 300 es
in vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
)";

const char * fragment_src = R"(#version 300 es
precision highp float;
uniform vec2 size;
uniform mat3 rot;      // eye orientation (camera -> world)
uniform vec4 fov;      // left, right, up, down tangents (all positive)
uniform int eye;       // 0 left, 1 right
uniform int variant;
out vec4 color;

float unwarp(float t) {
    if (t < 0.125) return sqrt(t / 2.0);
    if (t > 0.875) return 1.0 - sqrt((1.0 - t) / 2.0);
    return (t - 0.125) / 1.5 + 0.25;
}

float grid(float v, float step, float width) {
    float d = abs(fract(v / step + 0.5) - 0.5) * step;
    return 1.0 - smoothstep(0.0, width, d);
}

void main() {
    // DMA-BUF row 0 = top of the image = gl_FragCoord.y 0.5 (we render upside down on purpose)
    vec2 t = gl_FragCoord.xy / size;
    float s = unwarp(t.x);
    float r = unwarp(t.y);
    vec3 dir = normalize(vec3(mix(-fov.x, fov.y, s), mix(fov.z, -fov.w, r), -1.0));
    vec3 w = rot * dir;
    float lon = degrees(atan(w.x, -w.z));
    float lat = degrees(asin(clamp(w.y, -1.0, 1.0)));

    vec3 sky0 = variant == 1 ? vec3(0.6, 0.35, 0.85) : vec3(0.35, 0.55, 0.9);
    vec3 c = lat > 0.0 ? mix(sky0, vec3(0.05, 0.1, 0.35), lat / 90.0)
                       : mix(vec3(0.3, 0.45, 0.2), vec3(0.1, 0.15, 0.05), -lat / 90.0);
    float g = max(grid(lon, 10.0, 0.15), grid(lat, 10.0, 0.15));
    c = mix(c, vec3(0.9), g * 0.8);
    c = mix(c, vec3(1.0, 1.0, 0.2), 1.0 - smoothstep(0.0, 0.4, abs(lat)));
    if (abs(lon) < 2.0 && abs(lat) < 2.0) c = vec3(1.0, 0.1, 0.1);
    if (abs(abs(lon) - 90.0) < 1.0 && abs(lat) < 6.0) c = vec3(1.0, 0.6, 0.0);
    if (abs(abs(lon) - 180.0) < 1.5 && abs(lat) < 6.0) c = vec3(0.8, 0.0, 0.8);
    if (r > 0.30 && r < 0.33) {
        if (eye == 0 && s > 0.36 && s < 0.46) c = vec3(1.0, 0.0, 0.0);
        if (eye == 1 && s > 0.54 && s < 0.64) c = vec3(0.1, 0.3, 1.0);
    }
    color = vec4(c, 1.0);
}
)";

GLuint compile(GLenum type, const char * src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[2048];
		glGetShaderInfoLog(s, sizeof(log), nullptr, log);
		throw std::runtime_error(std::string("shader: ") + log);
	}
	return s;
}
} // namespace

struct vr_test_pattern::gl
{
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLContext context = EGL_NO_CONTEXT;
	GLuint program = 0, vbo = 0;
	struct target
	{
		EGLImageKHR image;
		GLuint texture, fbo;
	};
	std::map<int, target> targets; // by dma-buf fd
	PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture = nullptr;
};

vr_test_pattern::vr_test_pattern(gbm_allocator & alloc) :
        g(std::make_unique<gl>())
{
	auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	g->display = get_display ? get_display(EGL_PLATFORM_GBM_KHR, alloc.device(), nullptr) : EGL_NO_DISPLAY;
	if (g->display == EGL_NO_DISPLAY || !eglInitialize(g->display, nullptr, nullptr))
		throw std::runtime_error("EGL on GBM unavailable");
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
	g->context = eglCreateContext(g->display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
	if (g->context == EGL_NO_CONTEXT || !eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, g->context))
		throw std::runtime_error("cannot create a surfaceless GLES 3 context");
	g->create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
	g->destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
	g->image_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
	if (!g->create_image || !g->image_texture)
		throw std::runtime_error("EGL DMA-BUF import unavailable");

	g->program = glCreateProgram();
	glAttachShader(g->program, compile(GL_VERTEX_SHADER, vertex_src));
	glAttachShader(g->program, compile(GL_FRAGMENT_SHADER, fragment_src));
	glBindAttribLocation(g->program, 0, "pos");
	glLinkProgram(g->program);
	float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
	glGenBuffers(1, &g->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
}

vr_test_pattern::~vr_test_pattern()
{
	if (g->display == EGL_NO_DISPLAY)
		return;
	eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, g->context);
	for (auto & [fd, t]: g->targets)
	{
		glDeleteFramebuffers(1, &t.fbo);
		glDeleteTextures(1, &t.texture);
		g->destroy_image(g->display, t.image);
	}
	glDeleteBuffers(1, &g->vbo);
	glDeleteProgram(g->program);
	eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(g->display, g->context);
	eglTerminate(g->display);
}

void vr_test_pattern::render(const dmabuf_image & img, int eye, const vr_pose & pose, const fov_port & fov, int variant)
{
	eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, g->context);
	auto it = g->targets.find(img.fd);
	if (it == g->targets.end())
	{
		EGLint attr[] = {EGL_WIDTH, EGLint(img.width), EGL_HEIGHT, EGLint(img.height), EGL_LINUX_DRM_FOURCC_EXT, EGLint(img.fourcc),
		                 EGL_DMA_BUF_PLANE0_FD_EXT, img.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(img.offset),
		                 EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(img.stride),
		                 EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGLint(uint32_t(img.modifier)),
		                 EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGLint(uint32_t(img.modifier >> 32)), EGL_NONE};
		gl::target t{};
		t.image = g->create_image(g->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
		if (t.image == EGL_NO_IMAGE_KHR)
			throw std::runtime_error("cannot import the DMA-BUF into EGL");
		glGenTextures(1, &t.texture);
		glBindTexture(GL_TEXTURE_2D, t.texture);
		g->image_texture(GL_TEXTURE_2D, t.image);
		glGenFramebuffers(1, &t.fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			throw std::runtime_error("DMA-BUF is not renderable");
		it = g->targets.emplace(img.fd, t).first;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, it->second.fbo);
	glViewport(0, 0, GLint(img.width), GLint(img.height));
	glUseProgram(g->program);
	// quaternion -> column-major rotation matrix (camera -> world)
	auto [x, y, z, w] = pose.orientation;
	float m[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
	              2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
	              2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
	glUniformMatrix3fv(glGetUniformLocation(g->program, "rot"), 1, GL_FALSE, m);
	glUniform2f(glGetUniformLocation(g->program, "size"), float(img.width), float(img.height));
	glUniform4f(glGetUniformLocation(g->program, "fov"), fov.left, fov.right, fov.up, fov.down);
	glUniform1i(glGetUniformLocation(g->program, "eye"), eye);
	glUniform1i(glGetUniformLocation(g->program, "variant"), variant);
	glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glFinish(); // the encoder reads the buffer next
}
} // namespace od
