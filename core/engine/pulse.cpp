#include "pulse.h"

#include <pulse/pulseaudio.h>

#include <stdexcept>

namespace od
{
namespace
{
struct wait_state
{
	pa_threaded_mainloop * loop;
	bool done = false;
	bool ok = false;
	uint32_t index = PA_INVALID_INDEX;
	std::string text;
	std::optional<bool> flag;
};

void signal_success(pa_context *, int success, void * userdata)
{
	auto * w = static_cast<wait_state *>(userdata);
	w->ok = success;
	w->done = true;
	pa_threaded_mainloop_signal(w->loop, 0);
}
} // namespace

pulse_client::pulse_client()
{
	loop = pa_threaded_mainloop_new();
	ctx = pa_context_new(pa_threaded_mainloop_get_api(loop), "OpenDesk Streamer");
	pa_context_set_state_callback(
	        ctx, [](pa_context *, void * l) { pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop *>(l), 0); }, loop);
	pa_threaded_mainloop_lock(loop);
	if (pa_threaded_mainloop_start(loop) < 0 || pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
	{
		pa_threaded_mainloop_unlock(loop);
		throw std::runtime_error("cannot connect to the audio server");
	}
	for (;;)
	{
		pa_context_state_t state = pa_context_get_state(ctx);
		if (state == PA_CONTEXT_READY)
			break;
		if (!PA_CONTEXT_IS_GOOD(state))
		{
			pa_threaded_mainloop_unlock(loop);
			throw std::runtime_error("audio server connection failed");
		}
		pa_threaded_mainloop_wait(loop);
	}
	pa_threaded_mainloop_unlock(loop);
}

pulse_client::~pulse_client()
{
	if (loop)
		pa_threaded_mainloop_stop(loop);
	if (ctx)
	{
		pa_context_disconnect(ctx);
		pa_context_unref(ctx);
	}
	if (loop)
		pa_threaded_mainloop_free(loop);
}

template <typename F>
void pulse_client::run(F && start_operation)
{
	pa_threaded_mainloop_lock(loop);
	pa_operation * op = start_operation();
	if (op)
	{
		while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
			pa_threaded_mainloop_wait(loop);
		pa_operation_unref(op);
	}
	pa_threaded_mainloop_unlock(loop);
}

std::optional<uint32_t> pulse_client::load_module(const std::string & name, const std::string & args)
{
	wait_state w{loop};
	run([&] {
		return pa_context_load_module(
		        ctx, name.c_str(), args.c_str(),
		        [](pa_context *, uint32_t idx, void * u) {
			        auto * s = static_cast<wait_state *>(u);
			        s->index = idx;
			        pa_threaded_mainloop_signal(s->loop, 0);
		        },
		        &w);
	});
	if (w.index == PA_INVALID_INDEX)
		return std::nullopt;
	return w.index;
}

void pulse_client::unload_module(uint32_t index)
{
	wait_state w{loop};
	run([&] { return pa_context_unload_module(ctx, index, signal_success, &w); });
}

std::string pulse_client::default_sink()
{
	wait_state w{loop};
	run([&] {
		return pa_context_get_server_info(
		        ctx,
		        [](pa_context *, const pa_server_info * i, void * u) {
			        auto * s = static_cast<wait_state *>(u);
			        if (i && i->default_sink_name)
				        s->text = i->default_sink_name;
			        pa_threaded_mainloop_signal(s->loop, 0);
		        },
		        &w);
	});
	return w.text;
}

std::string pulse_client::default_source()
{
	wait_state w{loop};
	run([&] {
		return pa_context_get_server_info(
		        ctx,
		        [](pa_context *, const pa_server_info * i, void * u) {
			        auto * s = static_cast<wait_state *>(u);
			        if (i && i->default_source_name)
				        s->text = i->default_source_name;
			        pa_threaded_mainloop_signal(s->loop, 0);
		        },
		        &w);
	});
	return w.text;
}

void pulse_client::set_default_sink(const std::string & name)
{
	wait_state w{loop};
	run([&] { return pa_context_set_default_sink(ctx, name.c_str(), signal_success, &w); });
}

void pulse_client::set_default_source(const std::string & name)
{
	wait_state w{loop};
	run([&] { return pa_context_set_default_source(ctx, name.c_str(), signal_success, &w); });
}

std::optional<bool> pulse_client::sink_muted(const std::string & sink)
{
	wait_state w{loop};
	run([&] {
		return pa_context_get_sink_info_by_name(
		        ctx, sink.c_str(),
		        [](pa_context *, const pa_sink_info * i, int eol, void * u) {
			        auto * s = static_cast<wait_state *>(u);
			        if (i && !eol)
				        s->flag = i->mute != 0;
			        if (eol)
				        pa_threaded_mainloop_signal(s->loop, 0);
		        },
		        &w);
	});
	return w.flag;
}

void pulse_client::set_sink_mute(const std::string & sink, bool mute)
{
	wait_state w{loop};
	run([&] { return pa_context_set_sink_mute_by_name(ctx, sink.c_str(), mute, signal_success, &w); });
}
} // namespace od
