// An OS handle to shared GPU or memory objects: a file descriptor.
#pragma once

namespace od
{
using native_handle = int;
inline constexpr native_handle no_handle = -1;
} // namespace od
