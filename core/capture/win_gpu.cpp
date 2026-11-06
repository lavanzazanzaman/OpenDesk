#include "win_gpu.h"

#include <cstdio>
#include <cstdlib>
#include <dxgi1_6.h>
#include <format>
#include <vector>
#include <windows.h>

namespace od::win
{
namespace
{
std::vector<adapter_info> hardware_adapters()
{
	std::vector<adapter_info> out;
	IDXGIFactory1 * factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
		return out;
	IDXGIAdapter1 * a = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
	{
		DXGI_ADAPTER_DESC1 d{};
		if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
		{
			adapter_info info;
			info.luid = (uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
			info.vendor = d.VendorId;
			info.device = d.DeviceId;
			info.dedicated_vram = d.DedicatedVideoMemory;
			char name[256]{};
			WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
			info.name = name;
			out.push_back(info);
		}
		a->Release();
	}
	factory->Release();
	return out;
}

bool contains_nocase(const std::string & hay, const std::string & needle)
{
	auto lower = [](std::string s) {
		for (char & c: s)
			c = char(std::tolower(static_cast<unsigned char>(c)));
		return s;
	};
	return lower(hay).find(lower(needle)) != std::string::npos;
}
} // namespace

std::string node_for(uint64_t luid)
{
	return std::format("luid:{:016x}", luid);
}

std::optional<uint64_t> luid_of(const std::string & node)
{
	if (!node.starts_with("luid:") || node.size() != 5 + 16)
		return std::nullopt;
	return std::strtoull(node.c_str() + 5, nullptr, 16);
}

std::optional<adapter_info> adapter(const std::string & node)
{
	auto all = hardware_adapters();
	if (all.empty())
		return std::nullopt;
	if (auto luid = luid_of(node))
	{
		for (const auto & a: all)
			if (a.luid == *luid)
				return a;
		return std::nullopt;
	}
	return all.front();
}

std::string vr_node()
{
	auto all = hardware_adapters();
	if (all.empty())
		return {};
	if (const char * want = std::getenv("OPENDESK_VR_ADAPTER"); want && *want)
	{
		if (luid_of(want))
			return want;
		for (const auto & a: all)
			if (contains_nocase(a.name, want))
				return node_for(a.luid);
	}
	const adapter_info * best = &all.front();
	for (const auto & a: all)
		if (a.dedicated_vram > best->dedicated_vram)
			best = &a;
	return node_for(best->luid);
}
} // namespace od::win
