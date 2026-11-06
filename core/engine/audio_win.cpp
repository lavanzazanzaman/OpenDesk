#include "audio_win.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <ksmedia.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace od
{
bool send_all(int fd, const void * data, size_t size)
{
	const char * p = static_cast<const char *>(data);
	while (size)
	{
		int n = ::send(SOCKET(fd), p, int(std::min(size, size_t(1 << 20))), 0);
		if (n > 0)
		{
			p += n;
			size -= size_t(n);
			continue;
		}
		if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
		{
			fd_set write_set;
			FD_ZERO(&write_set);
			FD_SET(SOCKET(fd), &write_set);
			timeval tv{0, 100000};
			if (select(0, nullptr, &write_set, nullptr, &tv) < 0)
				return false;
			continue;
		}
		return false;
	}
	return true;
}

namespace
{
constexpr int sample_rate = 48000;
constexpr int channels = 2;
constexpr size_t chunk_frames = 240; // 5 ms

AVSampleFormat sample_format(const WAVEFORMATEX * fmt)
{
	if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		return AV_SAMPLE_FMT_FLT;
	if (fmt->wFormatTag == WAVE_FORMAT_PCM)
		return fmt->wBitsPerSample == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
	if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
	{
		const auto * ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
		if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
			return AV_SAMPLE_FMT_FLT;
		if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
			return fmt->wBitsPerSample == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
	}
	return AV_SAMPLE_FMT_FLT;
}

SwrContext * make_swr(const WAVEFORMATEX * in_format, int out_rate, int out_channels, AVSampleFormat out_sample_fmt)
{
	AVChannelLayout in_ch{}, out_ch{};
	av_channel_layout_default(&in_ch, in_format->nChannels);
	av_channel_layout_default(&out_ch, out_channels);
	SwrContext * swr = nullptr;
	if (swr_alloc_set_opts2(&swr, &out_ch, out_sample_fmt, out_rate, &in_ch, sample_format(in_format),
	                        int(in_format->nSamplesPerSec), 0, nullptr) < 0 ||
	    swr_init(swr) < 0)
	{
		swr_free(&swr);
		av_channel_layout_uninit(&in_ch);
		av_channel_layout_uninit(&out_ch);
		return nullptr;
	}
	av_channel_layout_uninit(&in_ch);
	av_channel_layout_uninit(&out_ch);
	return swr;
}

ComPtr<IMMDevice> default_device(IMMDeviceEnumerator * enumerator, EDataFlow flow)
{
	ComPtr<IMMDevice> device;
	if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device)))
		return device;
	return {};
}

// the endpoint whose name contains `wanted`; otherwise the default one, or none when fallback is false
ComPtr<IMMDevice> named_device(IMMDeviceEnumerator * enumerator, EDataFlow flow, const std::wstring & wanted, bool fallback = true)
{
	if (wanted.empty())
		return default_device(enumerator, flow);
	ComPtr<IMMDeviceCollection> collection;
	if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)))
		return {};
	UINT count = 0;
	collection->GetCount(&count);
	for (UINT i = 0; i < count; ++i)
	{
		ComPtr<IMMDevice> device;
		if (FAILED(collection->Item(i, &device)))
			continue;
		ComPtr<IPropertyStore> props;
		if (FAILED(device->OpenPropertyStore(STGM_READ, &props)))
			continue;
		PROPVARIANT value;
		PropVariantInit(&value);
		std::wstring name;
		if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
			name = value.pwszVal;
		PropVariantClear(&value);
		if (name.find(wanted) != std::wstring::npos)
			return device;
	}
	return fallback ? default_device(enumerator, flow) : ComPtr<IMMDevice>();
}

std::wstring mic_device_name()
{
	const char * env = getenv("OPENDESK_MIC_RENDER_DEVICE");
	std::string name = env && *env ? env : "CABLE Input";
	std::wstring wide;
	int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
	wide.resize(size_t(std::max(n - 1, 0)));
	MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), n);
	return wide;
}

// IPolicyConfig (Windows 7+): the interface the Sound control panel sets default devices with. Undocumented but
// stable; only SetDefaultEndpoint is used, the slots before it keep the vtable layout.
struct IPolicyConfig : IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX **) = 0;
	virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
	virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
constexpr IID iid_policy_config = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
constexpr CLSID clsid_policy_config = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

bool set_default_endpoint(const std::wstring & id)
{
	ComPtr<IPolicyConfig> policy;
	if (id.empty() || FAILED(CoCreateInstance(clsid_policy_config, nullptr, CLSCTX_ALL, iid_policy_config,
	                                          reinterpret_cast<void **>(policy.GetAddressOf()))))
		return false;
	bool ok = true;
	for (ERole role: {eConsole, eMultimedia, eCommunications})
		ok = SUCCEEDED(policy->SetDefaultEndpoint(id.c_str(), role)) && ok;
	return ok;
}

std::wstring endpoint_id(IMMDevice * device)
{
	std::wstring id;
	LPWSTR raw = nullptr;
	if (device && SUCCEEDED(device->GetId(&raw)))
		id = raw;
	CoTaskMemFree(raw);
	return id;
}

struct com_scope
{
	bool ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	~com_scope()
	{
		if (ok)
			CoUninitialize();
	}
};
} // namespace

std::string select_virtual_mic()
{
	com_scope com;
	ComPtr<IMMDeviceEnumerator> enumerator;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
		return {};
	// the cable's recording side: "CABLE Input" (where the headset mic is played) -> "CABLE Output"
	std::wstring wanted = mic_device_name();
	if (const size_t at = wanted.find(L"Input"); at != std::wstring::npos)
		wanted.replace(at, 5, L"Output");
	ComPtr<IMMDevice> cable = named_device(enumerator.Get(), eCapture, wanted, false);
	ComPtr<IMMDevice> current = default_device(enumerator.Get(), eCapture);
	const std::wstring cable_id = endpoint_id(cable.Get()), previous = endpoint_id(current.Get());
	if (cable_id.empty() || cable_id == previous || !set_default_endpoint(cable_id))
		return {};
	return previous.empty() ? std::string("-") : std::string(previous.begin(), previous.end()); // endpoint ids are ASCII
}

void restore_default_mic(const std::string & previous)
{
	if (previous.empty() || previous == "-")
		return;
	com_scope com;
	set_default_endpoint(std::wstring(previous.begin(), previous.end()));
}

std::string virtual_mic_device()
{
	const char * env = getenv("OPENDESK_MIC_RENDER_DEVICE");
	return env && *env ? env : "CABLE Input";
}

bool virtual_mic_available()
{
	const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	bool found = false;
	{
		ComPtr<IMMDeviceEnumerator> enumerator;
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
			found = bool(named_device(enumerator.Get(), eRender, mic_device_name(), false));
	}
	if (com)
		CoUninitialize();
	return found;
}

desktop_audio_sender::desktop_audio_sender(stream_channel & c, std::string d, const std::atomic<bool> & e) :
        channel(c), device(std::move(d)), enabled(e)
{
	thread = std::thread(&desktop_audio_sender::run, this);
}

desktop_audio_sender::~desktop_audio_sender()
{
	quit = true;
	if (thread.joinable())
		thread.join();
}

void desktop_audio_sender::run()
{
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool com = SUCCEEDED(hr);
	ComPtr<IMMDeviceEnumerator> enumerator;
	ComPtr<IMMDevice> endpoint;
	ComPtr<IAudioClient> client;
	ComPtr<IAudioCaptureClient> capture;
	ComPtr<IAudioClient> keepalive; // a silent stream on the same endpoint: loopback then delivers continuously
	ComPtr<IAudioRenderClient> keepalive_render;
	UINT32 keepalive_frames = 0;
	ComPtr<IAudioEndpointVolume> volume;
	bool muted_by_us = false;
	WAVEFORMATEX * mix = nullptr;
	SwrContext * swr = nullptr;
	uint8_t * converted = nullptr;
	int converted_size = 0;
	std::vector<uint8_t> pending;

	auto cleanup = [&] {
		if (client)
			client->Stop();
		if (keepalive)
			keepalive->Stop();
		if (muted_by_us && volume)
			volume->SetMute(FALSE, nullptr); // the PC's speakers as they were
		swr_free(&swr);
		av_freep(&converted);
		if (mix)
			CoTaskMemFree(mix);
		if (com)
			CoUninitialize();
	};

	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
	    !(endpoint = default_device(enumerator.Get(), eRender)))
	{
		cleanup();
		return;
	}
	if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(client.GetAddressOf()))) ||
	    FAILED(client->GetMixFormat(&mix)))
	{
		cleanup();
		return;
	}
	if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, mix, nullptr)) ||
	    FAILED(client->GetService(IID_PPV_ARGS(&capture))))
	{
		cleanup();
		return;
	}
	swr = make_swr(mix, sample_rate, channels, AV_SAMPLE_FMT_S16);
	if (!swr || FAILED(client->Start()))
	{
		cleanup();
		return;
	}
	// Windows only hands loopback data over while something plays: a silent stream keeps it running, so the headset
	// gets a continuous stream (silence included) as from PulseAudio/PipeWire on Linux
	if (SUCCEEDED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(keepalive.GetAddressOf()))) &&
	    SUCCEEDED(keepalive->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, mix, nullptr)) &&
	    SUCCEEDED(keepalive->GetBufferSize(&keepalive_frames)) && SUCCEEDED(keepalive->GetService(IID_PPV_ARGS(&keepalive_render))))
		keepalive->Start();
	else
	{
		keepalive.Reset();
		keepalive_render.Reset();
	}
	// "VR headset only": the PC's speakers are muted while streaming (restored at the end); loopback capture is taken
	// before the endpoint volume
	if (device == "mute-speakers" &&
	    SUCCEEDED(endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(volume.GetAddressOf()))))
	{
		BOOL already = FALSE;
		volume->GetMute(&already);
		if (!already && SUCCEEDED(volume->SetMute(TRUE, nullptr)))
			muted_by_us = true;
	}

	while (!quit)
	{
		if (keepalive_render) // top the silent stream up
		{
			UINT32 padding = 0;
			if (SUCCEEDED(keepalive->GetCurrentPadding(&padding)) && padding < keepalive_frames / 2)
			{
				BYTE * buffer = nullptr;
				const UINT32 frames = keepalive_frames - padding;
				if (SUCCEEDED(keepalive_render->GetBuffer(frames, &buffer)))
					keepalive_render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
			}
		}
		UINT32 available = 0;
		if (FAILED(capture->GetNextPacketSize(&available)) || !available)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			continue;
		}
		BYTE * data = nullptr;
		UINT32 frames = 0;
		DWORD flags = 0;
		if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
			continue;
		const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
		const int out_frames = int(av_rescale_rnd(swr_get_delay(swr, mix->nSamplesPerSec) + frames, sample_rate,
		                                          mix->nSamplesPerSec, AV_ROUND_UP));
		const int needed = out_frames * channels * int(sizeof(int16_t));
		if (needed > converted_size)
		{
			av_freep(&converted);
			if (av_samples_alloc(&converted, nullptr, channels, out_frames, AV_SAMPLE_FMT_S16, 0) < 0)
			{
				capture->ReleaseBuffer(frames);
				continue;
			}
			converted_size = needed;
		}
		const uint8_t * in[1] = {silent ? nullptr : data};
		if (silent) // silence is sent too: the stream stays continuous
		{
			const int64_t out = av_rescale_rnd(frames, sample_rate, mix->nSamplesPerSec, AV_ROUND_NEAR_INF);
			pending.insert(pending.end(), size_t(out) * channels * sizeof(int16_t), uint8_t(0));
		}
		else
		{
			int got = swr_convert(swr, &converted, out_frames, in, int(frames));
			if (got < 0)
				got = 0;
			pending.insert(pending.end(), converted, converted + size_t(got) * channels * sizeof(int16_t));
		}
		capture->ReleaseBuffer(frames);

		const size_t chunk_bytes = chunk_frames * channels * sizeof(int16_t);
		while (pending.size() >= chunk_bytes)
		{
			if (enabled && !paused)
			{
				std::vector<uint8_t> packet(4 + chunk_bytes);
				const int32_t size = int32_t(chunk_bytes);
				std::memcpy(packet.data(), &size, 4);
				std::memcpy(packet.data() + 4, pending.data(), chunk_bytes);
				channel.send(packet.data(), packet.size());
			}
			pending.erase(pending.begin(), pending.begin() + ptrdiff_t(chunk_bytes));
		}
	}
	cleanup();
}

mic_player::mic_player(float mic_volume)
{
	gain = mic_volume;
	thread = std::thread(&mic_player::run, this);
}

mic_player::~mic_player()
{
	{
		std::lock_guard lock(mutex);
		quit = true;
	}
	cv.notify_all();
	if (thread.joinable())
		thread.join();
}

void mic_player::push(const uint8_t * pcm, size_t size)
{
	if (!pcm || !size)
		return;
	{
		std::lock_guard lock(mutex);
		if (queue.size() > 100)
			queue.pop_front();
		queue.emplace_back(pcm, pcm + size);
	}
	cv.notify_one();
}

void mic_player::set_volume(float mic_volume) { gain = mic_volume; }

void mic_player::run()
{
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool com = SUCCEEDED(hr);
	ComPtr<IMMDeviceEnumerator> enumerator;
	ComPtr<IMMDevice> endpoint;
	ComPtr<IAudioClient> client;
	ComPtr<IAudioRenderClient> render;
	WAVEFORMATEX * mix = nullptr;
	SwrContext * swr = nullptr;
	uint8_t * converted = nullptr;
	int converted_size = 0;
	std::vector<int16_t> input;

	auto cleanup = [&] {
		if (client)
			client->Stop();
		swr_free(&swr);
		av_freep(&converted);
		if (mix)
			CoTaskMemFree(mix);
		if (com)
			CoUninitialize();
	};

	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
	    !(endpoint = named_device(enumerator.Get(), eRender, mic_device_name(), false)))
	{
		cleanup();
		return;
	}
	if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(client.GetAddressOf()))) ||
	    FAILED(client->GetMixFormat(&mix)))
	{
		cleanup();
		return;
	}
	if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, mix, nullptr)) ||
	    FAILED(client->GetService(IID_PPV_ARGS(&render))))
	{
		cleanup();
		return;
	}
	// Input is s16 mono 48 kHz, not a WAVEFORMATEX: build the converter for it directly.
	{
		AVChannelLayout in_ch{}, out_ch{};
		av_channel_layout_default(&in_ch, 1);
		av_channel_layout_default(&out_ch, mix->nChannels);
		if (swr_alloc_set_opts2(&swr, &out_ch, sample_format(mix), int(mix->nSamplesPerSec), &in_ch, AV_SAMPLE_FMT_S16,
		                        sample_rate, 0, nullptr) < 0 ||
		    swr_init(swr) < 0)
			swr_free(&swr);
		av_channel_layout_uninit(&in_ch);
		av_channel_layout_uninit(&out_ch);
	}
	UINT32 buffer_frames = 0;
	if (!swr || FAILED(client->GetBufferSize(&buffer_frames)) || FAILED(client->Start()))
	{
		cleanup();
		return;
	}
	const UINT32 period = std::max<UINT32>(buffer_frames / 4, 1);
	std::vector<uint8_t> silence(size_t(period) * mix->nBlockAlign, 0);

	while (true)
	{
		std::vector<uint8_t> chunk;
		{
			std::unique_lock lock(mutex);
			cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return quit || !queue.empty(); });
			if (quit)
				break;
			if (!queue.empty())
			{
				chunk = std::move(queue.front());
				queue.pop_front();
			}
		}
		BYTE * out = nullptr;
		if (FAILED(render->GetBuffer(period, &out)))
			break;
		std::memset(out, 0, size_t(period) * mix->nBlockAlign);
		if (!chunk.empty())
		{
			const int frames = int(chunk.size() / sizeof(int16_t));
			const int out_frames = int(av_rescale_rnd(swr_get_delay(swr, sample_rate) + frames, mix->nSamplesPerSec,
			                                          sample_rate, AV_ROUND_UP));
			const int needed = out_frames * int(mix->nChannels) * int(mix->wBitsPerSample / 8);
			if (needed > converted_size)
			{
				av_freep(&converted);
				if (av_samples_alloc(&converted, nullptr, int(mix->nChannels), out_frames, sample_format(mix), 0) < 0)
				{
					render->ReleaseBuffer(period, 0);
					continue;
				}
				converted_size = needed;
			}
			const uint8_t * in[1] = {chunk.data()};
			int got = swr_convert(swr, &converted, out_frames, in, frames);
			if (got > 0)
			{
				const float g = gain.load();
				if (sample_format(mix) == AV_SAMPLE_FMT_FLT)
				{
					auto * samples = reinterpret_cast<float *>(converted);
					for (int i = 0; i < got * int(mix->nChannels); ++i)
						samples[i] *= g;
				}
				const size_t bytes = std::min<size_t>(size_t(got) * size_t(mix->nChannels) * av_get_bytes_per_sample(sample_format(mix)),
		                                       size_t(period) * mix->nBlockAlign);
				std::memcpy(out, converted, bytes);
			}
		}
		render->ReleaseBuffer(period, 0);
		std::this_thread::sleep_for(std::chrono::microseconds(period * 1000000ull / std::max<UINT32>(mix->nSamplesPerSec, 1)));
	}
	cleanup();
}
} // namespace od
