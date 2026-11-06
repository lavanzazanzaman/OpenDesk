// Linux build of VDXR (MIT): replacements for the Windows-only pieces the portable
// code touches. ETW tracing becomes a no-op, a few CRT/Win32 helpers map to POSIX, graphics is Vulkan only.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

// --- ETW TraceLogging -> nothing (arguments are never evaluated)
#define TRACELOGGING_DECLARE_PROVIDER(name) extern int name
#define TRACELOGGING_DEFINE_PROVIDER(name, ...) int name = 0
template <auto & provider>
struct TraceLoggingActivity
{
};
#define TraceLoggingWrite(...) ((void)0)
#define TraceLoggingWriteStart(...) ((void)0)
#define TraceLoggingWriteStop(...) ((void)0)
#define TraceLoggingWriteTagged(...) ((void)0)
#define TraceLoggingProviderEnabled(...) false
#define TraceLoggingRegister(...) ((void)0)
#define TraceLoggingUnregister(...) ((void)0)

// --- CRT / Win32 helpers
#define _TRUNCATE ((size_t)-1)
#define vsnprintf_s(buf, size, count, fmt, va) vsnprintf(buf, size, fmt, va)
#define sprintf_s(buf, size, ...) snprintf(buf, size, __VA_ARGS__)
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
inline void OutputDebugStringA(const char * s) { fputs(s, stderr); }
#define __declspec(x)
#define XRAPI_EXPORT __attribute__((visibility("default")))
#define _ReturnAddress() __builtin_return_address(0)

// --- Win32 result type (used by the XrUtility helpers)
using HRESULT = int32_t;
#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define _snprintf_s(buf, size, count, ...) snprintf(buf, size, __VA_ARGS__)

// --- plain Win32 types that appear in otherwise portable declarations
using UINT = uint32_t;
using UINT64 = uint64_t;
using DWORD = uint32_t;
struct LUID
{
	uint32_t LowPart;
	int32_t HighPart;
};
union LARGE_INTEGER {
	struct
	{
		uint32_t LowPart;
		int32_t HighPart;
	};
	int64_t QuadPart;
};
// WIL RAII handles: placeholders on Linux (the code using them is Windows-only)
namespace wil
{
struct unique_handle
{
	void reset() {}
	explicit operator bool() const { return false; }
};
using shared_handle = unique_handle;
using unique_hmodule = unique_handle;
using unique_registry_watcher = unique_handle;
} // namespace wil

#define HKEY_LOCAL_MACHINE 0
#define HKEY_CURRENT_USER 1
