// Minimal synchronous libpulse client (served by pipewire-pulse) for device management:
// virtual sinks/sources (module-null-sink), default sink/source, sink mute. Replaces shelling out to pactl.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct pa_threaded_mainloop;
struct pa_context;

namespace od
{
class pulse_client
{
public:
	pulse_client(); // throws if no server
	~pulse_client();
	pulse_client(const pulse_client &) = delete;
	pulse_client & operator=(const pulse_client &) = delete;

	// returns the module index
	std::optional<uint32_t> load_module(const std::string & name, const std::string & args);
	void unload_module(uint32_t index);

	std::string default_sink();
	std::string default_source();
	void set_default_sink(const std::string &);
	void set_default_source(const std::string &);

	std::optional<bool> sink_muted(const std::string & sink);
	void set_sink_mute(const std::string & sink, bool mute);

private:
	template <typename F>
	void run(F && start_operation);
	pa_threaded_mainloop * loop = nullptr;
	pa_context * ctx = nullptr;
};
} // namespace od
