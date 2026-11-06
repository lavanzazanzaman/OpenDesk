// Runtime HLSL compilation for the Windows build (tools/hlsl_embed.py): d3dcompiler_47.dll, part of Windows 10+.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <d3dcompiler.h>

namespace od_hlsl
{
struct blob
{
	std::vector<uint8_t> code;
	const void * data() const { return code.data(); }
	size_t size() const { return code.size(); }
};

inline blob compile(const char * source, size_t size, const char * name, const char * profile)
{
	blob out;
	static HMODULE lib = LoadLibraryW(L"d3dcompiler_47.dll");
	using compile_fn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
	                                     ID3DBlob **, ID3DBlob **);
	auto d3d_compile = lib ? reinterpret_cast<compile_fn>(reinterpret_cast<void *>(GetProcAddress(lib, "D3DCompile"))) : nullptr;
	if (!d3d_compile)
	{
		OutputDebugStringA("OpenDesk XR: d3dcompiler_47.dll unavailable\n");
		return out;
	}
	ID3DBlob * code = nullptr;
	ID3DBlob * errors = nullptr;
	const HRESULT hr = d3d_compile(source, size, name, nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (errors)
	{
		OutputDebugStringA(static_cast<const char *>(errors->GetBufferPointer()));
		errors->Release();
	}
	if (SUCCEEDED(hr) && code)
	{
		const auto * p = static_cast<const uint8_t *>(code->GetBufferPointer());
		out.code.assign(p, p + code->GetBufferSize());
	}
	if (code)
		code->Release();
	return out;
}
} // namespace od_hlsl
