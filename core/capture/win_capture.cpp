#include "win_capture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace od
{
namespace
{
// the cursor on the GPU: its shape as a BGRA texture (monochrome / masked shapes converted), drawn alpha-blended into
// the frame copy with a quad whose viewport is the cursor's rectangle
const char * cursor_shader = R"(
Texture2D<float4> cursor : register(t0);
SamplerState point_clamp : register(s0);
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
	uv = float2((id << 1) & 2, id & 2);
	pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	return cursor.Sample(point_clamp, uv);
}
)";

ComPtr<ID3DBlob> compile(const char * entry, const char * profile)
{
	using compile_fn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
	                                     ID3DBlob **, ID3DBlob **);
	static HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
	auto fn = lib ? reinterpret_cast<compile_fn>(reinterpret_cast<void *>(GetProcAddress(lib, "D3DCompile"))) : nullptr;
	ComPtr<ID3DBlob> code, errors;
	if (fn)
		fn(cursor_shader, strlen(cursor_shader), "cursor.hlsl", nullptr, nullptr, entry, profile, 0, 0, &code, &errors);
	return code;
}
} // namespace

struct win_capture::impl
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGIOutputDuplication> duplication;
	ComPtr<ID3D11Texture2D> frame; // our copy of the desktop (the cursor is drawn into it), what the encoder reads
	ComPtr<ID3D11RenderTargetView> frame_rtv;
	DXGI_OUTPUT_DESC output{};
	int width = 0, height = 0;
	bool logged_rotation = false;
	// cursor
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> ps;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11BlendState> blend;
	ComPtr<ID3D11Texture2D> cursor;
	ComPtr<ID3D11ShaderResourceView> cursor_srv;
	int cursor_w = 0, cursor_h = 0, hot_x = 0, hot_y = 0;
	bool cursor_tried = false;
};

win_capture::win_capture(std::string d, int f, frame_callback cb, std::function<void(std::string)> l) :
        device(std::move(d)), fps(f), on_frame(std::move(cb)), log(std::move(l)), state(std::make_unique<impl>())
{
	thread = std::thread(&win_capture::run, this);
}

win_capture::~win_capture()
{
	quit = true;
	if (thread.joinable())
		thread.join();
}

void win_capture::close()
{
	auto & s = *state;
	s.frame_rtv.Reset();
	s.frame.Reset();
	s.cursor_srv.Reset();
	s.cursor.Reset();
	s.vs.Reset();
	s.ps.Reset();
	s.sampler.Reset();
	s.blend.Reset();
	s.duplication.Reset();
	s.context.Reset();
	s.device.Reset();
}

bool win_capture::open()
{
	auto & s = *state;
	close();
	// the adapter that drives this monitor (hybrid laptops / several GPUs: not necessarily the default one)
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(factory.GetAddressOf()))))
		return false;
	const std::wstring wanted(device.begin(), device.end());
	ComPtr<IDXGIAdapter1> adapter;
	ComPtr<IDXGIOutput> output;
	for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
		for (UINT i = 0;; ++i)
		{
			ComPtr<IDXGIOutput> o;
			if (adapter->EnumOutputs(i, &o) == DXGI_ERROR_NOT_FOUND)
				break;
			DXGI_OUTPUT_DESC desc{};
			if (SUCCEEDED(o->GetDesc(&desc)) && wanted == desc.DeviceName)
			{
				output = o;
				s.output = desc;
				break;
			}
		}
	if (!output)
		return false;
	D3D_FEATURE_LEVEL level{};
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
	// video support: the encoder converts the frames with this device's video processor
	if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
	                             levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &s.device, &level, &s.context)))
		return false;
	// capture and encoder threads share the device
	ComPtr<ID3D10Multithread> mt;
	if (SUCCEEDED(s.device.As(&mt)))
		mt->SetMultithreadProtected(TRUE);
	ComPtr<IDXGIOutput1> output1;
	if (FAILED(output.As(&output1)) || FAILED(output1->DuplicateOutput(s.device.Get(), &s.duplication)))
		return false;
	s.width = s.output.DesktopCoordinates.right - s.output.DesktopCoordinates.left;
	s.height = s.output.DesktopCoordinates.bottom - s.output.DesktopCoordinates.top;
	s.frame.Reset();
	s.cursor.Reset();
	s.cursor_tried = false;
	return true;
}

void win_capture::run()
{
	using clock = std::chrono::steady_clock;
	const auto min_interval = std::chrono::microseconds(1000000 / std::max(fps, 1));
	auto last = clock::now();
	bool reported = false;
	int failures = 0;
	while (!quit)
	{
		if (!state->duplication && !open())
		{
			if (!reported)
				log("[capture] cannot open DXGI Desktop Duplication for " + device + " (retrying)");
			reported = true;
			if (++failures > 20)
			{
				failed_ = true;
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}
		reported = false;
		failures = 0;

		DXGI_OUTDUPL_FRAME_INFO info{};
		ComPtr<IDXGIResource> resource;
		const HRESULT hr = state->duplication->AcquireNextFrame(50, &info, &resource);
		if (hr == DXGI_ERROR_WAIT_TIMEOUT)
			continue;
		if (FAILED(hr))
		{
			close();
			continue;
		}
		ComPtr<ID3D11Texture2D> texture;
		if (FAILED(resource.As(&texture)))
		{
			state->duplication->ReleaseFrame();
			continue;
		}
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		if (!state->frame || state->width != int(desc.Width) || state->height != int(desc.Height))
		{
			D3D11_TEXTURE2D_DESC copy = desc;
			copy.Usage = D3D11_USAGE_DEFAULT;
			copy.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			copy.CPUAccessFlags = 0;
			copy.MiscFlags = 0;
			state->frame_rtv.Reset();
			if (FAILED(state->device->CreateTexture2D(&copy, nullptr, &state->frame)) ||
			    FAILED(state->device->CreateRenderTargetView(state->frame.Get(), nullptr, &state->frame_rtv)))
			{
				state->duplication->ReleaseFrame();
				close();
				continue;
			}
			state->width = int(desc.Width);
			state->height = int(desc.Height);
		}
		// on the GPU from here: the desktop image, then the cursor drawn into it; the encoder reads `frame`
		state->context->CopyResource(state->frame.Get(), texture.Get());
		if (info.LastMouseUpdateTime.QuadPart && info.PointerShapeBufferSize)
			update_cursor_shape();
		if (info.PointerPosition.Visible || cursor_visible)
		{
			if (info.LastMouseUpdateTime.QuadPart)
			{
				cursor_visible = info.PointerPosition.Visible;
				cursor_x = info.PointerPosition.Position.x;
				cursor_y = info.PointerPosition.Position.y;
			}
			if (cursor_visible)
				draw_cursor();
		}
		state->duplication->ReleaseFrame();

		if (state->output.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
		    state->output.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED && !state->logged_rotation)
		{
			state->logged_rotation = true;
			log("[capture] rotated monitor: the image is streamed in the panel's native orientation");
		}
		captured_frame frame;
		frame.d3d11_device = state->device.Get();
		frame.d3d11_texture = state->frame.Get();
		frame.av_sw_format = AV_PIX_FMT_BGRA;
		on_frame(frame, state->width, state->height);

		last += min_interval;
		if (const auto now = clock::now(); last > now)
			std::this_thread::sleep_until(last);
		else
			last = now;
	}
	close();
}

// the pointer shape -> a BGRA texture (DXGI hands it over only when it changes)
void win_capture::update_cursor_shape()
{
	auto & s = *state;
	UINT size = 0;
	DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
	if (s.duplication->GetFramePointerShape(0, nullptr, &size, &shape) != DXGI_ERROR_MORE_DATA || !size)
		return;
	std::vector<uint8_t> raw(size);
	if (FAILED(s.duplication->GetFramePointerShape(size, raw.data(), &size, &shape)))
		return;
	const bool mono = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
	const int w = int(shape.Width), h = mono ? int(shape.Height) / 2 : int(shape.Height);
	if (w <= 0 || h <= 0)
		return;
	std::vector<uint8_t> bgra(size_t(w) * h * 4, 0);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			uint8_t * out = &bgra[(size_t(y) * w + x) * 4];
			if (mono)
			{
				// AND 1 + XOR 1 inverts the screen: drawn white (no read of the destination in this pass)
				const uint8_t bit = uint8_t(0x80 >> (x & 7));
				const bool a = raw[size_t(y) * shape.Pitch + size_t(x >> 3)] & bit;
				const bool v = raw[size_t(h + y) * shape.Pitch + size_t(x >> 3)] & bit;
				const uint8_t c = v ? 255 : 0;
				out[0] = out[1] = out[2] = c;
				out[3] = (!a || v) ? 255 : 0;
			}
			else
			{
				const uint8_t * in = &raw[size_t(y) * shape.Pitch + size_t(x) * 4];
				std::memcpy(out, in, 4);
				if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) // mask 0: opaque color, else XOR (skipped)
					out[3] = in[3] ? (in[0] | in[1] | in[2] ? 255 : 0) : 255;
			}
		}
	D3D11_TEXTURE2D_DESC td{};
	td.Width = UINT(w);
	td.Height = UINT(h);
	td.MipLevels = td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA data{bgra.data(), UINT(w * 4), 0};
	s.cursor_srv.Reset();
	s.cursor.Reset();
	if (SUCCEEDED(s.device->CreateTexture2D(&td, &data, &s.cursor)))
		s.device->CreateShaderResourceView(s.cursor.Get(), nullptr, &s.cursor_srv);
	s.cursor_w = w;
	s.cursor_h = h;
	s.hot_x = int(shape.HotSpot.x);
	s.hot_y = int(shape.HotSpot.y);
}

void win_capture::draw_cursor()
{
	auto & s = *state;
	if (!s.cursor_srv)
		return;
	if (!s.vs && !s.cursor_tried)
	{
		s.cursor_tried = true;
		auto vs_code = compile("vs", "vs_5_0");
		auto ps_code = compile("ps", "ps_5_0");
		if (vs_code && ps_code)
		{
			s.device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &s.vs);
			s.device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &s.ps);
		}
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		s.device->CreateSamplerState(&sd, &s.sampler);
		D3D11_BLEND_DESC bd{};
		bd.RenderTarget[0].BlendEnable = TRUE;
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		s.device->CreateBlendState(&bd, &s.blend);
		if (!s.vs || !s.ps)
			log("[capture] no cursor shaders (d3dcompiler_47.dll?): the mouse pointer is not drawn");
	}
	if (!s.vs || !s.ps)
		return;
	D3D11_VIEWPORT vp{float(cursor_x - s.hot_x), float(cursor_y - s.hot_y), float(s.cursor_w), float(s.cursor_h), 0, 1};
	auto * ctx = s.context.Get();
	ctx->OMSetRenderTargets(1, s.frame_rtv.GetAddressOf(), nullptr);
	ctx->OMSetBlendState(s.blend.Get(), nullptr, 0xffffffff);
	ctx->RSSetViewports(1, &vp);
	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(s.vs.Get(), nullptr, 0);
	ctx->PSSetShader(s.ps.Get(), nullptr, 0);
	ctx->PSSetShaderResources(0, 1, s.cursor_srv.GetAddressOf());
	ctx->PSSetSamplers(0, 1, s.sampler.GetAddressOf());
	ctx->Draw(3, 0);
	ID3D11RenderTargetView * none = nullptr;
	ctx->OMSetRenderTargets(1, &none, nullptr);
}
} // namespace od
