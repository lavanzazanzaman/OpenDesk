// Local network adapters advertised to the headset (Computer.PrivateAdapters).
#pragma once

#include "protocol.h"

#include <vector>

namespace od
{
// IPv4 on normal interfaces; link-local IPv6 on USB (NCM) links, which the headset uses with its own scope id.
std::vector<network_adapter> local_adapters();
} // namespace od
