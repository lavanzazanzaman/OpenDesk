#pragma once
#include "hardware_encoder.h"
#include <memory>
#include <vector>

namespace od {
// Modifier-aware download for encoder backends which cannot import DRM PRIME.
// Owns its EGL context and restores the caller's context after every operation.
class dmabuf_readback {
public:
    explicit dmabuf_readback(const std::string & node);
    ~dmabuf_readback();
    const uint8_t * read(const captured_frame &, int width, int height);
private:
    struct impl;
    std::unique_ptr<impl> state;
};
}
