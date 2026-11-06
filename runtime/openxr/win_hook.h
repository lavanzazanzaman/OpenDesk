// Windows: the one function hook upstream installs with Microsoft Detours (OpenEventW, for OVRPlugin's ovr_Detect),
// done by patching the import tables of the loaded modules instead - no Detours in the MinGW build. Same interface as
// utils.h's DetourDllAttach / DetourDllDetach, which use these.
#pragma once

#include <windows.h>

namespace opendesk_openxr::win_hook {
    // replaces every loaded module's import of dll!name with `hook`; returns the original function (nullptr: none)
    void* attach(const char* dll, const char* name, void* hook);
    void detach(const char* dll, const char* name, void* hook, void* original);
} // namespace opendesk_openxr::win_hook

inline LONG DetourRestoreAfterWith() {
    return NO_ERROR;
}
