// Makes OpenDesk XR the system's OpenXR runtime while the streamer runs (the loader
// reads ~/.config/openxr/1/active_runtime[.x86_64].json). Reversible:
//   activate(): copies the runtime into ~/.local/share/opendesk/openxr (visible inside Steam's pressure-vessel
//               container), backs up the user's active_runtime files to ~/.local/state/opendesk/openxr-backup and
//               points both at our manifest
//   restore():  puts the backed-up files back (also run at startup if a previous run did not get to it)
#pragma once

#include <string>

namespace od::openxr_runtime
{
// runtime_dir: where libopendesk-openxr.so + libovr-vd.so were built. Returns an error message or "".
std::string activate(const std::string & runtime_dir);
void restore();
bool active(); // our manifest is the active runtime right now
std::string installed_manifest();
} // namespace od::openxr_runtime

// Same for OpenVR games: xrizer (OpenVR on top of OpenXR - i.e. on OpenDesk XR) becomes the OpenVR runtime while the streamer
// runs. OpenVR reads ~/.config/openvr/openvrpaths.vrpath ("runtime": [dirs]); the file is backed up to
// ~/.local/state/opendesk/openvr-backup and only its "runtime" entry is changed. The runtime is copied to
// ~/.local/share/opendesk/xrizer (what the registration points at; a running game keeps its copy on update).
namespace od::openvr_runtime
{
// Only our own xrizer is used - never whatever is installed system-wide: tools/get_xrizer.sh installs xrizer's CI
// nightly (or a source build) to built_xrizer_dir(); OPENDESK_XRIZER overrides it.
std::string built_xrizer_dir();  // ~/.local/share/opendesk/xrizer-build
std::string xrizer_version();    // its version.txt ("nightly <date> sha256:…" / "source <commit>"), "" if not installed
std::string find_xrizer();       // OPENDESK_XRIZER or built_xrizer_dir() if it holds a runtime, else ""
std::string activate(const std::string & xrizer_dir); // error message or ""
void restore();
bool active();
std::string installed_dir();
} // namespace od::openvr_runtime

// Foveated rendering (a Vulkan layer, removed): earlier versions installed an implicit layer manifest in
// ~/.local/share/vulkan/implicit_layer.d and its library in ~/.local/share/opendesk/vulkan - deleted at startup
namespace od::foveated_layer
{
void remove_leftovers();
} // namespace od::foveated_layer
