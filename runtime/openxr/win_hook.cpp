// Import-table patching for win_hook.h (Windows only).
#ifdef _WIN32
#include "pch.h"

#include <psapi.h>

namespace opendesk_openxr::win_hook {
    namespace {
        // every module's import thunk for dll!name (matched by the function address it currently holds)
        template <class F>
        void for_each_thunk(void* current, F f) {
            HMODULE modules[1024];
            DWORD needed = 0;
            if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
                return;
            }
            const size_t count = std::min<size_t>(needed / sizeof(HMODULE), std::size(modules));
            for (size_t m = 0; m < count; ++m) {
                auto* base = reinterpret_cast<uint8_t*>(modules[m]);
                auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
                    continue;
                }
                auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
                if (!dir.VirtualAddress) {
                    continue;
                }
                for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
                    for (auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk); thunk->u1.Function; ++thunk) {
                        if (reinterpret_cast<void*>(thunk->u1.Function) == current) {
                            f(reinterpret_cast<void**>(&thunk->u1.Function));
                        }
                    }
                }
            }
        }

        void write(void** slot, void* value) {
            DWORD old = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                *slot = value;
                VirtualProtect(slot, sizeof(void*), old, &old);
            }
        }

        // forwarders (kernel32 -> kernelbase): modules may import either
        std::vector<void*> addresses(const char* dll, const char* name) {
            std::vector<void*> out;
            for (const char* lib : {dll, "kernelbase.dll"}) {
                if (HMODULE h = GetModuleHandleA(lib)) {
                    if (void* p = reinterpret_cast<void*>(GetProcAddress(h, name))) {
                        if (std::find(out.begin(), out.end(), p) == out.end()) {
                            out.push_back(p);
                        }
                    }
                }
            }
            return out;
        }
    } // namespace

    void* attach(const char* dll, const char* name, void* hook) {
        const auto targets = addresses(dll, name);
        if (targets.empty()) {
            return nullptr;
        }
        for (void* target : targets) {
            for_each_thunk(target, [&](void** slot) { write(slot, hook); });
        }
        return targets.front();
    }

    void detach(const char*, const char*, void* hook, void* original) {
        for_each_thunk(hook, [&](void** slot) { write(slot, original); });
    }
} // namespace opendesk_openxr::win_hook
#endif
