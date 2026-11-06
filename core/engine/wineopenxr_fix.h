// Proton's WineOpenXR frees a destroyed session's wrapper without unlinking it from its session list
// (wineopenxr/openxr_loader.c xrDestroySession: free() without list_remove, Proton 9 to 11). xrPollEvent walks that
// list for every session event, so an app that ever destroyed a session reads freed memory and eventually crashes:
// Oculus games through RiftLift's (Revive) bridge, which creates and destroys probe sessions before the real one, die
// ~30 s in (access violation in wineopenxr.dll xrPollEvent).
//
// patch_wineopenxr() fixes a wineopenxr.dll in place: xrDestroySession's success path jumps to a few instructions in
// the .text padding that unlink the wrapper under session_list_lock, then free and return like the original. All
// addresses come from the DLL's COFF symbols and every expected instruction is verified first; any other build is left
// untouched. tools/fix_wineopenxr.py is the same patch as a script.
#pragma once

#include <string>

namespace od
{
enum class wineopenxr_state
{
	patched,      // fixed (by us)
	fixed,        // this build unlinks sessions itself (e.g. GE-Proton 11)
	needs_patch,  // the known bug, patchable
	unsupported,  // not a build we recognise: left alone
	error,        // unreadable
};

wineopenxr_state check_wineopenxr(const std::string & dll, std::string * detail = nullptr);
bool patch_wineopenxr(const std::string & dll, std::string * error = nullptr); // true if patched now or already
} // namespace od
