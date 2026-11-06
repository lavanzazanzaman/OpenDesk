#include "win_capture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
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
void blend_pixel(uint8_t * dst, const uint8_t * src)
{
	const int a = src[3];
	if (!a)
		return;
	const int inv = 255 - a;
	dst[0] = uint8_t((src[0] * a + dst[0] * inv) / 255);
	dst[1] = uint8_t((src[1] * a + dst[1] * inv) / 255);
	dst[2] = uint8_t((src[2] * a + dst[2] * inv) / 255);
}
} // namespace

struct win_capture::impl
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGIOutputDuplication> duplication;
	ComPtr<ID3D11Texture2D> staging;
	DXGI_OUTPUT_DESC output{};
	int width = 0, height = 0;
	bool logged_rotation = false;
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
	s.staging.Reset();
	s.duplication.Reset();
	s.context.Reset();
	s.device.Reset();
}

bool win_capture::open()
{
	auto & s = *state;
	close();
	D3D_FEATURE_LEVEL level{};
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
	                             UINT(std::size(levels)), D3D11_SDK_VERSION, &s.device, &level, &s.context)))
		return false;
	ComPtr<IDXGIDevice> dxgi_device;
	ComPtr<IDXGIAdapter> adapter;
	if (FAILED(s.device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)))
		return false;
	const std::wstring wanted(device.begin(), device.end());
	for (UINT i = 0;; ++i)
	{
		ComPtr<IDXGIOutput> output;
		if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_OUTPUT_DESC desc{};
		if (FAILED(output->GetDesc(&desc)) || wanted != desc.DeviceName)
			continue;
		s.output = desc;
		ComPtr<IDXGIOutput1> output1;
		if (FAILED(output.As(&output1)) || FAILED(output1->DuplicateOutput(s.device.Get(), &s.duplication)))
			return false;
		s.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
		s.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
		return true;
	}
	return false;
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
		if (!state->staging || state->width != int(desc.Width) || state->height != int(desc.Height))
		{
			D3D11_TEXTURE2D_DESC staging = desc;
			staging.Usage = D3D11_USAGE_STAGING;
			staging.BindFlags = 0;
			staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			staging.MiscFlags = 0;
			if (FAILED(state->device->CreateTexture2D(&staging, nullptr, &state->staging)))
			{
				state->duplication->ReleaseFrame();
				close();
				continue;
			}
			state->width = int(desc.Width);
			state->height = int(desc.Height);
		}
		state->context->CopyResource(state->staging.Get(), texture.Get());
		D3D11_MAPPED_SUBRESOURCE map{};
		if (FAILED(state->context->Map(state->staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
		{
			state->duplication->ReleaseFrame();
			continue;
		}

		if (state->output.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
		    state->output.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED && !state->logged_rotation)
		{
			state->logged_rotation = true;
			log("[capture] rotated monitor: the image is streamed in the panel's native orientation");
		}
		if (info.PointerPosition.Visible)
		{
			grab_pointer(map.pData, int(map.RowPitch), state->width, state->height, info.PointerPosition.Position.x,
			             info.PointerPosition.Position.y);
		}

		captured_frame frame;
		frame.data = static_cast<const uint8_t *>(map.pData);
		frame.stride = uint32_t(map.RowPitch);
		frame.size = size_t(map.RowPitch) * size_t(state->height);
		frame.av_sw_format = AV_PIX_FMT_BGRA;
		on_frame(frame, state->width, state->height);

		state->context->Unmap(state->staging.Get(), 0);
		state->duplication->ReleaseFrame();

		last += min_interval;
		if (const auto now = clock::now(); last > now)
			std::this_thread::sleep_until(last);
		else
			last = now;
	}
	close();
}

bool win_capture::grab_pointer(void * mapped, int stride, int width, int height, int pointer_x, int pointer_y)
{
	UINT size = 0;
	DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
	HRESULT hr = state->duplication->GetFramePointerShape(0, nullptr, &size, &shape);
	if (hr != DXGI_ERROR_MORE_DATA || !size)
		return false;
	std::vector<uint8_t> pixels(size);
	if (FAILED(state->duplication->GetFramePointerShape(size, pixels.data(), &size, &shape)))
		return false;

	const int left = pointer_x - int(shape.HotSpot.x);
	const int top = pointer_y - int(shape.HotSpot.y);
	auto * dst = static_cast<uint8_t *>(mapped);

	if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME)
	{
		const int mask_height = int(shape.Height);
		for (int y = 0; y < mask_height; ++y)
		{
			const int dy = top + y;
			if (dy < 0 || dy >= height)
				continue;
			const uint8_t * and_row = pixels.data() + size_t(y) * shape.Pitch;
			const uint8_t * xor_row = pixels.data() + size_t(mask_height + y) * shape.Pitch;
			for (int x = 0; x < int(shape.Width); ++x)
			{
				const int dx = left + x;
				if (dx < 0 || dx >= width)
					continue;
				const uint8_t bit = uint8_t(0x80 >> (x & 7));
				const bool a = (and_row[x >> 3] & bit) != 0;
				const bool v = (xor_row[x >> 3] & bit) != 0;
				auto * pixel = dst + size_t(dy) * size_t(stride) + size_t(dx) * 4;
				if (a && v)
				{
					pixel[0] = uint8_t(255 - pixel[0]);
					pixel[1] = uint8_t(255 - pixel[1]);
					pixel[2] = uint8_t(255 - pixel[2]);
				}
				else if (!a)
				{
					pixel[0] = pixel[1] = pixel[2] = v ? 255 : 0;
				}
			}
		}
		return true;
	}

	// COLOR and MASKED_COLOR: 32-bit BGRA, alpha blended.
	for (int y = 0; y < int(shape.Height); ++y)
	{
		const int dy = top + y;
		if (dy < 0 || dy >= height)
			continue;
		const uint8_t * src = pixels.data() + size_t(y) * shape.Pitch;
		for (int x = 0; x < int(shape.Width); ++x)
		{
			const int dx = left + x;
			if (dx < 0 || dx >= width)
				continue;
			blend_pixel(dst + size_t(dy) * size_t(stride) + size_t(dx) * 4, src + size_t(x) * 4);
		}
	}
	return true;
}
} // namespace od
