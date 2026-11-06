#include "dmabuf_readback.h"
#include "dmabuf.h"
#include "test_pattern.h"
#include <EGL/egl.h>
#include <gbm.h>
#include <cstdio>
#include <stdexcept>

int main() try {
    od::gbm_allocator allocator(od::default_render_node());
    od::vr_test_pattern context(allocator);
    const auto before = eglGetCurrentContext();
    auto image = allocator.allocate(64, 48);
    uint32_t stride = 0; void *mapping = nullptr;
    auto *pixels = static_cast<unsigned char*>(gbm_bo_map(image->bo, 0, 0, 64, 48, GBM_BO_TRANSFER_WRITE, &stride, &mapping));
    if (!pixels) throw std::runtime_error("mapping failed");
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) {
        auto *p = pixels + y * stride + x * 4;
        p[0] = 19; p[1] = y * 4; p[2] = x * 3; p[3] = 255;
    }
    gbm_bo_unmap(image->bo, mapping);
    od::captured_frame frame;
    frame.fd = image->fd; frame.drm_format = image->fourcc; frame.modifier = image->modifier;
    frame.offset = image->offset; frame.stride = image->stride;
    od::dmabuf_readback reader(od::default_render_node());
    for (int i = 0; i < 2; ++i) {
        auto *result = reader.read(frame, 64, 48);
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) {
            const auto *p = result + (y * 64 + x) * 4;
            if (p[0] != x * 3 || p[1] != y * 4 || p[2] != 19 || p[3] != 255)
                throw std::runtime_error("RGBA order / row orientation / stride mismatch");
        }
        if (eglGetCurrentContext() != before) throw std::runtime_error("caller EGL context not restored");
    }
    puts("ALL PASS DMA-BUF download: every RGBA pixel, orientation, stride, repeat, caller EGL context");
} catch (const std::exception &e) { fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
