// ETW TraceLogging -> nothing (the Linux build, and the Windows build with MinGW, whose headers lack TraceLogging).
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
#define TraceLoggingWriteStart(...) ((void)0)
#define TraceLoggingWriteStop(...) ((void)0)
#define TraceLoggingWriteTagged(...) ((void)0)
#define TraceLoggingProviderEnabled(...) false
#define TraceLoggingRegister(...) ((void)0)
#define TraceLoggingUnregister(...) ((void)0)

