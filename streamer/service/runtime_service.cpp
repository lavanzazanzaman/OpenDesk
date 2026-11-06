// OpenDesk Service (Windows): makes OpenDesk XR the system's OpenXR runtime while the streamer runs, and puts the
// previous runtime back afterwards - the HKLM value the OpenXR loader reads, which the streamer (a normal user process)
// may not write. Runs as LocalSystem, installed by the installer.
//
// Pipe \\.\pipe\opendesk-service (authenticated users): one text command per message, a one-line reply.
//   openxr-activate  ActiveRuntime = <this service's folder>\runtime\opendesk_xr.json (the previous value is kept)
//   openxr-restore   the kept value again, if ActiveRuntime is still ours
//   status           "active" / "inactive"
// Nothing a caller sends is used as a path or value: the only runtime it can register is the one installed next to it.
//
//   opendesk-service.exe            (started by the service manager)
//   opendesk-service.exe --console  (runs the pipe server in a console, for testing)
//   opendesk-service.exe --call status|openxr-activate|openxr-restore  (asks the running service, prints the reply)
#include <windows.h>
#include <sddl.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
constexpr const wchar_t * service_name = L"OpenDeskService";
constexpr const wchar_t * pipe_name = L"\\\\.\\pipe\\opendesk-service";
constexpr const wchar_t * khronos_key = L"SOFTWARE\\Khronos\\OpenXR\\1";
constexpr const wchar_t * our_key = L"SOFTWARE\\OpenDesk";

SERVICE_STATUS_HANDLE status_handle = nullptr;
SERVICE_STATUS status{SERVICE_WIN32_OWN_PROCESS, SERVICE_START_PENDING, 0, NO_ERROR, 0, 0, 0};
HANDLE stop_event = nullptr;

std::wstring read_value(const wchar_t * key, const wchar_t * name)
{
	wchar_t buffer[2048]{};
	DWORD size = sizeof(buffer) - sizeof(wchar_t);
	if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size) != ERROR_SUCCESS)
		return {};
	return buffer;
}

bool write_value(const wchar_t * key, const wchar_t * name, const std::wstring & value)
{
	HKEY k = nullptr;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS)
		return false;
	const LSTATUS r =
	        RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
	RegCloseKey(k);
	return r == ERROR_SUCCESS;
}

std::wstring our_manifest()
{
	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring dir(path);
	dir.resize(dir.find_last_of(L'\\'));
	return dir + L"\\runtime\\opendesk_xr.json";
}

bool same(const std::wstring & a, const std::wstring & b)
{
	return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

std::string handle(const std::string & command)
{
	const std::wstring ours = our_manifest();
	const std::wstring current = read_value(khronos_key, L"ActiveRuntime");
	if (command == "status")
		return same(current, ours) ? "active" : "inactive";
	if (command == "openxr-activate")
	{
		if (GetFileAttributesW(ours.c_str()) == INVALID_FILE_ATTRIBUTES)
			return "error: OpenDesk XR is not installed next to the service";
		if (same(current, ours))
			return "ok";
		if (!current.empty())
			write_value(our_key, L"PreviousOpenXRRuntime", current);
		return write_value(khronos_key, L"ActiveRuntime", ours) ? "ok" : "error: cannot write the OpenXR runtime";
	}
	if (command == "openxr-restore")
	{
		if (!same(current, ours))
			return "ok"; // someone else changed it meanwhile: leave theirs
		const std::wstring previous = read_value(our_key, L"PreviousOpenXRRuntime");
		if (previous.empty() || same(previous, ours))
			return "ok"; // nothing to go back to: OpenDesk XR stays
		return write_value(khronos_key, L"ActiveRuntime", previous) ? "ok" : "error: cannot write the OpenXR runtime";
	}
	return "error: unknown command";
}

void serve()
{
	// authenticated users may connect (the streamer runs as the signed-in user); SYSTEM and administrators fully
	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
	ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GRGW;;;AU)(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1,
	                                                      &sa.lpSecurityDescriptor, nullptr);
	OVERLAPPED ov{};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0)
	{
		HANDLE pipe = CreateNamedPipeW(pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
		                               PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 4, 512, 512, 0, &sa);
		if (pipe == INVALID_HANDLE_VALUE)
		{
			Sleep(1000);
			continue;
		}
		ResetEvent(ov.hEvent);
		bool connected = ConnectNamedPipe(pipe, &ov) || GetLastError() == ERROR_PIPE_CONNECTED;
		if (!connected && GetLastError() == ERROR_IO_PENDING)
		{
			HANDLE waits[2] = {ov.hEvent, stop_event};
			if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
			{
				CancelIoEx(pipe, &ov);
				CloseHandle(pipe);
				break;
			}
			DWORD n;
			connected = GetOverlappedResult(pipe, &ov, &n, FALSE);
		}
		if (connected)
		{
			char buffer[64]{};
			DWORD n = 0;
			OVERLAPPED rov{};
			rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &n, &rov) || (GetLastError() == ERROR_IO_PENDING &&
			                                                               WaitForSingleObject(rov.hEvent, 2000) == WAIT_OBJECT_0 &&
			                                                               GetOverlappedResult(pipe, &rov, &n, FALSE)))
			{
				std::string command(buffer, n);
				while (!command.empty() && (command.back() == '\n' || command.back() == '\r' || command.back() == '\0'))
					command.pop_back();
				const std::string reply = handle(command);
				DWORD written = 0;
				ResetEvent(rov.hEvent);
				if (!WriteFile(pipe, reply.data(), DWORD(reply.size()), &written, &rov) && GetLastError() == ERROR_IO_PENDING)
					WaitForSingleObject(rov.hEvent, 2000);
				FlushFileBuffers(pipe);
			}
			CloseHandle(rov.hEvent);
		}
		DisconnectNamedPipe(pipe);
		CloseHandle(pipe);
	}
	CloseHandle(ov.hEvent);
	LocalFree(sa.lpSecurityDescriptor);
}

void set_state(DWORD state)
{
	status.dwCurrentState = state;
	status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
	SetServiceStatus(status_handle, &status);
}

DWORD WINAPI control(DWORD code, DWORD, void *, void *)
{
	if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN)
	{
		set_state(SERVICE_STOP_PENDING);
		SetEvent(stop_event);
	}
	return NO_ERROR;
}

void WINAPI service_main(DWORD, LPWSTR *)
{
	status_handle = RegisterServiceCtrlHandlerExW(service_name, control, nullptr);
	if (!status_handle)
		return;
	stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	set_state(SERVICE_RUNNING);
	serve();
	set_state(SERVICE_STOPPED);
}
} // namespace

int wmain(int argc, wchar_t ** argv)
{
	if (argc > 2 && std::wstring(argv[1]) == L"--call")
	{
		char command[64]{};
		WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, command, sizeof(command) - 1, nullptr, nullptr);
		if (!WaitNamedPipeW(pipe_name, 2000))
		{
			std::printf("the service is not running\n");
			return 1;
		}
		HANDLE h = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		DWORD mode = PIPE_READMODE_MESSAGE;
		SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
		char reply[128]{};
		DWORD n = 0;
		const BOOL ok = TransactNamedPipe(h, command, DWORD(strlen(command)), reply, sizeof(reply) - 1, &n, nullptr);
		CloseHandle(h);
		std::printf("%s\n", ok ? reply : "no reply");
		return ok && std::strncmp(reply, "error", 5) != 0 ? 0 : 1;
	}
	if (argc > 1 && std::wstring(argv[1]) == L"--console")
	{
		stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		std::printf("serving %ls (Ctrl+C to stop)\n", pipe_name);
		serve();
		return 0;
	}
	SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(service_name), service_main}, {nullptr, nullptr}};
	return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
