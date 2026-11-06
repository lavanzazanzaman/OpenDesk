// Windows GPU selection. Windows has no DRM render nodes; the "render node" strings the engine passes around are
// "luid:<16 hex digits>" there, naming a DXGI adapter by its LUID (empty: the adapter Windows lists first, which drives
// the primary display).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace od::win
{
struct adapter_info
{
	uint64_t luid = 0; // LUID as HighPart << 32 | LowPart
	unsigned vendor = 0, device = 0;
	uint64_t dedicated_vram = 0;
	std::string name;
};

std::string node_for(uint64_t luid);
std::optional<uint64_t> luid_of(const std::string & node);
// the adapter a node names (empty node: the first hardware adapter)
std::optional<adapter_info> adapter(const std::string & node);
// the GPU for VR: OPENDESK_VR_ADAPTER (a substring of the adapter name, or a luid: node), else the hardware adapter
// with the most dedicated video memory (the discrete GPU of a hybrid laptop)
std::string vr_node();
} // namespace od::win
