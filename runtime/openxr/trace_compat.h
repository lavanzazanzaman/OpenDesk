// ETW TraceLogging -> nothing (not available on Linux).
// Arguments are never evaluated.
#pragma once

// --- ETW TraceLogging -> nothing (arguments are never evaluated)
#define TRACELOGGING_DECLARE_PROVIDER(name) extern int name
#define TRACELOGGING_DEFINE_PROVIDER(name, ...) int name = 0
template <auto & provider>
struct TraceLoggingActivity
{
};
#define TraceLoggingWrite(...) ((void)0)
// the first call of each traced function leaves one line in the log: where an app stops, the last line shows
#include <mutex>
namespace opendesk_openxr::log {
    void Log(const char* fmt, ...);
}
#define TraceLoggingWriteStart(activity, name, ...)                                                                    \
    do {                                                                                                               \
        static std::once_flag odFirstCall_;                                                                            \
        std::call_once(odFirstCall_, [] { ::opendesk_openxr::log::Log("first call: %s\n", name); });                   \
    } while (0)
#define TraceLoggingWriteStop(...) ((void)0)
#define TraceLoggingWriteTagged(...) ((void)0)
#define TraceLoggingProviderEnabled(...) false
#define TraceLoggingRegister(...) ((void)0)
#define TraceLoggingUnregister(...) ((void)0)

