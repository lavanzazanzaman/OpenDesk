// vd-wineopenxr-fix [--check] path/to/wineopenxr.dll  (exit 0: patched now/already, 2: --check and not patched, 1: error)
#include "wineopenxr_fix.h"

#include <cstdio>
#include <string>

int main(int argc, char ** argv)
{
	bool check = argc == 3 && std::string(argv[1]) == "--check";
	if (argc != (check ? 3 : 2))
	{
		std::fprintf(stderr, "usage: %s [--check] wineopenxr.dll\n", argv[0]);
		return 1;
	}
	std::string dll = argv[argc - 1], detail;
	auto state = od::check_wineopenxr(dll, &detail);
	std::printf("%s\n", detail.c_str());
	bool fine = state == od::wineopenxr_state::patched || state == od::wineopenxr_state::fixed;
	if (check || fine)
		return fine ? 0 : state == od::wineopenxr_state::needs_patch ? 2 : 1;
	if (!od::patch_wineopenxr(dll, &detail))
	{
		std::printf("failed: %s\n", detail.c_str());
		return 1;
	}
	std::printf("patched\n");
	return 0;
}
