// Compiles the runtime's embedded HLSL shaders with d3dcompiler_47.dll, like OpenDesk XR does at session start.
//   hlsl-compile-test   (Windows; prints each shader's bytecode size, exit 1 if one fails)
#include "AlphaBlendingCS.h"
#include "FullScreenQuadVS.h"
#include "ResolveMultisampledDepthPS.h"
#include "SharpeningCS.h"
#include "UpscalingCS.h"

#include <cstdio>

int main()
{
	struct
	{
		const char * name;
		const od_hlsl::blob & blob;
	} shaders[] = {{"AlphaBlendingCS", g_AlphaBlendingCS_blob()},
	               {"FullScreenQuadVS", g_FullScreenQuadVS_blob()},
	               {"ResolveMultisampledDepthPS", g_ResolveMultisampledDepthPS_blob()},
	               {"SharpeningCS", g_SharpeningCS_blob()},
	               {"UpscalingCS", g_UpscalingCS_blob()}};
	int failed = 0;
	for (const auto & s: shaders)
	{
		std::printf("%-28s %s (%zu bytes)\n", s.name, s.blob.size() ? "ok" : "FAILED", s.blob.size());
		failed += s.blob.size() == 0;
	}
	return failed ? 1 : 0;
}
