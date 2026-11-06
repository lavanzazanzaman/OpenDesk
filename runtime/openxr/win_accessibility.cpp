// Windows (MinGW build): upstream's controller emulation for accessibility drives virtual controllers through the
// GameInput SDK, which the MinGW toolchain has no headers for. Not built: no helper, as on Linux.
#ifdef _WIN32
#include "pch.h"

#include "accessibility.h"

namespace opendesk_openxr {
    std::unique_ptr<AccessibilityHelper> CreateAccessibilityHelper(ovrSession, const std::wstring&, const std::string&, const std::string&) {
        return nullptr;
    }
} // namespace opendesk_openxr
#endif
