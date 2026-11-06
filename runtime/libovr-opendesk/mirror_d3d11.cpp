#ifdef _WIN32
#include "mirror_d3d11.h"

#include <OVR_CAPI.h>
#include <d3dcompiler.h>

#include <cstring>

namespace od::ovr
{
namespace
{
// full-screen triangle; the pixel shader maps the target onto the source eye's viewport (and array slice)
const char * shader_source = R"(
cbuffer view : register(b0)
{
	float4 rect;    // source u0, v0, u1, v1
	float4 target;  // target x0, x1 (normalized: which part of the mirror this eye fills), slice, flip
};
Texture2DArray<float4> eye : register(t0);
SamplerState linear_clamp : register(s0);

void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
	uv = float2((id << 1) & 2, id & 2);
	pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	float u = (uv.x - target.x) / (target.y - target.x);
	if (u < 0 || u > 1)
		discard;
	float v = target.w > 0.5 ? 1 - uv.y : uv.y;
	return float4(eye.Sample(linear_clamp, float3(lerp(rect.x, rect.z, u), lerp(rect.y, rect.w, v), target.z)).rgb, 1);
}
)";

struct view_constants
{
	float rect[4];
	float target[4];
};

using compile_fn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                     ID3DBlob **, ID3DBlob **);

ID3DBlob * compile(const char * entry, const char * profile)
{
	static HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
	auto fn = lib ? reinterpret_cast<compile_fn>(reinterpret_cast<void *>(GetProcAddress(lib, "D3DCompile"))) : nullptr;
	if (!fn)
		return nullptr;
	ID3DBlob * code = nullptr;
	ID3DBlob * errors = nullptr;
	fn(shader_source, std::strlen(shader_source), "mirror.hlsl", nullptr, nullptr, entry, profile, 0, 0, &code, &errors);
	if (errors)
	{
		report("mirror shader: %s", static_cast<const char *>(errors->GetBufferPointer()));
		errors->Release();
	}
	return code;
}

template <class T>
void release(T *& p)
{
	if (p)
		p->Release();
	p = nullptr;
}
} // namespace

mirror_d3d11::mirror_d3d11(ID3D11Device * dev, uint32_t w, uint32_t h, DXGI_FORMAT texture_format, DXGI_FORMAT view_format, unsigned opts) :
        width(w), height(h), options(opts)
{
	D3D11_TEXTURE2D_DESC td{};
	td.Width = w;
	td.Height = h;
	td.MipLevels = td.ArraySize = 1;
	td.Format = texture_format;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex)))
		return;
	if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&device))))
		return; // a black mirror then: the texture exists
	ID3D11DeviceContext1 * immediate = nullptr;
	device->GetImmediateContext1(&immediate);
	context = immediate;
	const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
	const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
	if (FAILED(device->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &state)))
		return;
	D3D11_RENDER_TARGET_VIEW_DESC rd{};
	rd.Format = view_format;
	rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	if (FAILED(device->CreateRenderTargetView(tex, &rd, &rtv)))
		return;
	ID3DBlob * vs_code = compile("vs", "vs_5_0");
	ID3DBlob * ps_code = compile("ps", "ps_5_0");
	if (vs_code && ps_code)
	{
		device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs);
		device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps);
	}
	release(vs_code);
	release(ps_code);
	D3D11_SAMPLER_DESC sd{};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	device->CreateSamplerState(&sd, &sampler);
	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = sizeof(view_constants);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	device->CreateBuffer(&bd, nullptr, &constants);
	ready = vs && ps && sampler && constants;
	if (!ready)
		report("mirror texture: no shaders (d3dcompiler_47.dll?) - it stays black");
}

mirror_d3d11::~mirror_d3d11()
{
	release(constants);
	release(sampler);
	release(ps);
	release(vs);
	release(rtv);
	release(state);
	release(context);
	release(device);
	release(tex);
}

void mirror_d3d11::update(const std::vector<composite_layer> & layers)
{
	if (!ready)
		return;
	const composite_layer * projection = nullptr;
	for (const auto & l: layers)
		if (l.type == composite_layer::projection)
		{
			projection = &l;
			break;
		}
	if (!projection)
		return;
	// the eyes the options ask for: left / right only, or both side by side
	int eyes[2] = {0, 1};
	int eye_count = 2;
	if (options & ovrMirrorOption_LeftEyeOnly)
		eye_count = 1;
	else if (options & ovrMirrorOption_RightEyeOnly)
		eyes[0] = 1, eye_count = 1;

	ID3DDeviceContextState * previous = nullptr;
	context->SwapDeviceContextState(state, &previous);
	const float black[4] = {0, 0, 0, 1};
	context->ClearRenderTargetView(rtv, black);
	D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
	context->RSSetViewports(1, &vp);
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vs, nullptr, 0);
	context->PSSetShader(ps, nullptr, 0);
	context->PSSetSamplers(0, 1, &sampler);
	context->PSSetConstantBuffers(0, 1, &constants);
	for (int k = 0; k < eye_count; ++k)
	{
		const int e = eyes[k];
		const swapchain * sc = projection->chain[e];
		if (!sc || sc->committed < 0 || size_t(sc->committed) >= sc->textures.size())
			continue;
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = DXGI_FORMAT(sc->dxgi_view_format);
		sd.ViewDimension = sc->samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		if (sc->samples > 1)
			continue; // multisampled eye textures: not mirrored (the stream is unaffected)
		sd.Texture2DArray.MipLevels = 1;
		sd.Texture2DArray.ArraySize = sc->layers;
		ID3D11ShaderResourceView * srv = nullptr;
		if (FAILED(device->CreateShaderResourceView(sc->textures[size_t(sc->committed)], &sd, &srv)))
			continue;
		view_constants c{};
		std::memcpy(c.rect, projection->rect[e], sizeof(c.rect));
		c.target[0] = float(k) / float(eye_count);
		c.target[1] = float(k + 1) / float(eye_count);
		c.target[2] = float(projection->array_index[e]);
		c.target[3] = projection->origin_bottom_left ? 1.f : 0.f;
		context->UpdateSubresource(constants, 0, nullptr, &c, 0, 0);
		context->PSSetShaderResources(0, 1, &srv);
		context->Draw(3, 0);
		ID3D11ShaderResourceView * none = nullptr;
		context->PSSetShaderResources(0, 1, &none);
		srv->Release();
	}
	context->SwapDeviceContextState(previous, nullptr);
	if (previous)
		previous->Release();
}
} // namespace od::ovr
#endif
