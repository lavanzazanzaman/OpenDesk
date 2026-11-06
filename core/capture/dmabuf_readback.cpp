#include "dmabuf_readback.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>

namespace od {
namespace {
struct restore_context {
    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    EGLenum api = eglQueryAPI();
    EGLDisplay temporary;
    ~restore_context() {
        eglMakeCurrent(temporary, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglBindAPI(api);
        if (display != EGL_NO_DISPLAY) eglMakeCurrent(display, draw, read, context);
    }
};
}
struct dmabuf_readback::impl {
    int fd = -1;
    gbm_device * gbm = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    std::vector<uint8_t> pixels;
    ~impl() {
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (display != EGL_NO_DISPLAY) eglTerminate(display);
        if (gbm) gbm_device_destroy(gbm);
        if (fd >= 0) close(fd);
    }
};
dmabuf_readback::dmabuf_readback(const std::string & node) : state(std::make_unique<impl>()) {
    auto & s = *state;
    s.fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (s.fd < 0 || !(s.gbm = gbm_create_device(s.fd))) throw std::runtime_error("DMA-BUF readback: cannot open " + node);
    s.display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, s.gbm, nullptr);
    if (s.display == EGL_NO_DISPLAY || !eglInitialize(s.display, nullptr, nullptr)) throw std::runtime_error("DMA-BUF readback: EGL initialization failed");
    restore_context restore{.temporary = s.display};
    eglBindAPI(EGL_OPENGL_ES_API);
    // GBM exposes window configs, often no PBUFFER configs. We use no surface.
    EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
    EGLConfig config; EGLint count = 0;
    if (!eglChooseConfig(s.display, attributes, &config, 1, &count) || !count) throw std::runtime_error("DMA-BUF readback: no EGL config");
    EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    s.context = eglCreateContext(s.display, config, EGL_NO_CONTEXT, context_attributes);
    if (s.context == EGL_NO_CONTEXT) throw std::runtime_error("DMA-BUF readback: no EGL context");
}
dmabuf_readback::~dmabuf_readback() = default;
const uint8_t * dmabuf_readback::read(const captured_frame & f, int width, int height) {
    auto & s = *state;
    restore_context restore{.temporary = s.display};
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, s.context)) throw std::runtime_error("DMA-BUF readback: make current failed");
    auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!create_image || !destroy_image || !image_target) throw std::runtime_error("DMA-BUF readback: EGL image extension missing");
    std::vector<EGLint> a{EGL_WIDTH, width, EGL_HEIGHT, height, EGL_LINUX_DRM_FOURCC_EXT, EGLint(f.drm_format),
        EGL_DMA_BUF_PLANE0_FD_EXT, f.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(f.offset), EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(f.stride)};
    if (f.modifier != ~uint64_t(0)) a.insert(a.end(), {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGLint(f.modifier), EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGLint(f.modifier >> 32)});
    a.push_back(EGL_NONE);
    EGLImageKHR img = create_image(s.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a.data());
    if (img == EGL_NO_IMAGE_KHR) throw std::runtime_error("DMA-BUF readback: image import failed");
    GLuint texture = 0, fbo = 0;
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture); image_target(GL_TEXTURE_2D, img);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        s.pixels.resize(size_t(width) * height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, s.pixels.data());
        ok = glGetError() == GL_NO_ERROR;
    }
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &texture); destroy_image(s.display, img);
    if (!ok) throw std::runtime_error("DMA-BUF readback: framebuffer download failed");
    return s.pixels.data();
}
}
