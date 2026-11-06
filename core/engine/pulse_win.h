// Windows stub for the PulseAudio device-management client. Windows audio routing uses the default
// endpoint; virtual sinks/sources require a separate virtual audio driver.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace od
{
class pulse_client
{
public:
	pulse_client() = default;
	~pulse_client() = default;
	pulse_client(const pulse_client &) = delete;
	pulse_client & operator=(const pulse_client &) = delete;

	std::optional<uint32_t> load_module(const std::string &, const std::string &) { return std::nullopt; }
	void unload_module(uint32_t) {}
	std::string default_sink() { return {}; }
	std::string default_source() { return {}; }
	void set_default_sink(const std::string &) {}
	void set_default_source(const std::string &) {}
	std::optional<bool> sink_muted(const std::string &) { return std::nullopt; }
	void set_sink_mute(const std::string &, bool) {}
};
} // namespace od
