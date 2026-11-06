#ifdef _WIN32 // vr_ipc.cpp on Linux
#include "vr_ipc.h"

#include <windows.h>
#include <sddl.h>

namespace od
{
int64_t ipc::monotonic_ns()
{
	static const int64_t frequency = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return int64_t(f.QuadPart);
	}();
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	// split to keep the multiplication within 64 bits for any uptime
	const int64_t seconds = now.QuadPart / frequency, rest = now.QuadPart % frequency;
	return seconds * 1000000000 + rest * 1000000000 / frequency;
}

std::wstring ipc::pipe_name()
{
	std::wstring sid = L"unknown";
	HANDLE token = nullptr;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
	{
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::string buffer(size, '\0');
		if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
		{
			LPWSTR text = nullptr;
			if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text))
			{
				sid = text;
				LocalFree(text);
			}
		}
		CloseHandle(token);
	}
	return L"\\\\.\\pipe\\opendesk-vr-" + sid;
}

bool ipc::service_available()
{
	// WaitNamedPipe fails at once with ERROR_FILE_NOT_FOUND when no instance exists; a busy pipe still exists
	if (WaitNamedPipeW(pipe_name().c_str(), 1))
		return true;
	return GetLastError() == ERROR_SEM_TIMEOUT;
}
} // namespace od
#endif
