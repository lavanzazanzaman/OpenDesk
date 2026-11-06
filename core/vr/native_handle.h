// An OS handle to shared GPU or memory objects: a file descriptor on Linux, an NT handle on Windows.
#pragma once

namespace od
{
#ifdef _WIN32
using native_handle = void *;
inline constexpr native_handle no_handle = nullptr;
#else
using native_handle = int;
inline constexpr native_handle no_handle = -1;
#endif
} // namespace od
