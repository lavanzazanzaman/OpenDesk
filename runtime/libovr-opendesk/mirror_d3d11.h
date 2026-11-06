// Windows: LibOVR mirror textures (ovr_CreateMirrorTextureWithOptionsDX) with the VR view in them - the eye(s) the
// options ask for, from the frame's projection layer, scaled into the texture. OpenDesk XR's mirror window shows it.
// Drawn with a full-screen triangle in a context state of its own, so the app's D3D11 pipeline state is untouched.
#pragma once
#ifdef _WIN32

#include "compositor.h"

#include <d3d11_1.h>

#include <memory>
#include <vector>

namespace od::ovr
{
class mirror_d3d11
{
public:
	// format: the typed DXGI format to render in; options: ovrMirrorOptions (eye selection)
	mirror_d3d11(ID3D11Device *, uint32_t width, uint32_t height, DXGI_FORMAT texture_format, DXGI_FORMAT view_format, unsigned options);
	~mirror_d3d11();
	ID3D11Texture2D * texture() const { return tex; }
	// draws the frame's first projection layer (D3D11 swapchains only)
	void update(const std::vector<composite_layer> &);

private:
	ID3D11Device1 * device = nullptr;
	ID3D11DeviceContext1 * context = nullptr;
	ID3DDeviceContextState * state = nullptr;
	ID3D11Texture2D * tex = nullptr;
	ID3D11RenderTargetView * rtv = nullptr;
	ID3D11VertexShader * vs = nullptr;
	ID3D11PixelShader * ps = nullptr;
	ID3D11SamplerState * sampler = nullptr;
	ID3D11Buffer * constants = nullptr;
	uint32_t width = 0, height = 0;
	unsigned options = 0;
	bool ready = false;
};
} // namespace od::ovr
#endif
