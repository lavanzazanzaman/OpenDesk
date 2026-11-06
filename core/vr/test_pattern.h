// Built-in VR test scene (port of engine/vdvr.py, verified on a Quest 3): a world-locked sky/ground grid at infinity,
// a red marker at world forward, orange at left/right, purple behind, and fixed per-eye markers (red bar left of
// centre in the LEFT eye, blue bar right of centre in the RIGHT eye). Rendered straight into the defoveation-warped
// layout with GLES on the encoder GPU (EGL on GBM, no window), into DMA-BUF images.
#pragma once

#include "dmabuf.h"
#include "vr_types.h"

#include <map>

namespace od
{
class vr_test_pattern
{
public:
	explicit vr_test_pattern(gbm_allocator &);
	~vr_test_pattern();
	// variant 1 = external test client (purple sky), to tell it apart from the built-in pattern in the headset
	void render(const dmabuf_image & target, int eye, const vr_pose &, const fov_port &, int variant = 0); // returns when done

private:
	struct gl;
	std::unique_ptr<gl> g;
};
} // namespace od
