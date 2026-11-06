// video_test split FILE h264|hevc        -> number of access units (compare with ffprobe)
#include "gamepad.h"
#include "input.h"
#include "pipewire_capture.h"
#include "portal.h"
#include "kwin_screencast.h"
#include "displays.h"
#include "desktop_video.h"
#include "file_player.h"
#include "vr_sender.h"
#include "games.h"
#include <QFile>
#include <QGuiApplication>
#include <cmath>
#include <QTimer>
#include <QEventLoop>
#include <format>
#include <map>
#include <sys/socket.h>
#include <cstring>
#include "hardware_encoder.h"
#include "video.h"

extern "C" {
#include <libavutil/log.h>
}
#include <QCoreApplication>
#include <mutex>

int vd_test_bgr0();
#include <QTimer>
#include <QEventLoop>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace od;

int main(int argc, char ** argv)
{
	std::string cmd = argc > 1 ? argv[1] : "";
	if (cmd == "games") // games [OUTDIR]: Games tab scan + running state (+ thumbnails as <platform>-<id>.png)
	{
		QGuiApplication app(argc, argv); // fonts for generated cards
		od::process_table procs;
		for (const auto & g: od::scan_games())
		{
			std::cout << std::format("{} {:>18} {:<7} {}", int(g.platform), g.app_id, procs.running(g) ? "running" : "-",
			                         g.name.toStdString())
			          << std::endl;
			if (argc > 2)
			{
				QFile f(QString("%1/%2-%3.png").arg(argv[2]).arg(int(g.platform)).arg(qulonglong(g.app_id)));
				if (f.open(QIODevice::WriteOnly))
					f.write(od::game_thumbnail(g));
			}
		}
		return 0;
	}
	if (cmd == "split")
	{
		std::ifstream f(argv[2], std::ios::binary);
		bytes data((std::istreambuf_iterator<char>(f)), {});
		auto codec = std::string(argv[3]) == "hevc" ? video_codec::hevc : video_codec::h264;
		access_unit_splitter s(codec);
		size_t count = 0, sync = 0;
		for (size_t i = 0; i < data.size(); i += 7777) // odd piece size on purpose
			s.push(data.data() + i, std::min<size_t>(7777, data.size() - i), [&](bytes && au) { ++count; sync += is_sync(au, codec); });
		if (s.flush())
			++count;
		std::cout << count << " " << sync << "\n";
		return 0;
	}
	if (cmd == "format") // VideoFormat + Slice layout for 2 screens, hex
	{
		std::vector<stream_format> f{{3440, 1440, {0, 0, 3440, 1440}}, {1920, 1080, {3440, 0, 1920, 1080}}};
		for (const bytes & b: {pack_video_format(0, video_codec::hevc, 90, f), pack_slice_header(0x0102030405060708, 1, 0x1234)})
		{
			for (uint8_t c: b)
				std::printf("%02x", c);
			std::printf("\n");
		}
		return 0;
	}
	if (cmd == "displays") // read-only: KWin screen-cast availability + outputs (creates no stream)
	{
		std::string why;
		auto k = kwin_screencast::connect(&why);
		if (!k)
		{
			std::printf("kwin screencast: unavailable (%s)\n", why.c_str());
			return 1;
		}
		for (const auto & d: k->displays())
			std::printf("%s '%s' logical %d,%d %dx%d pixels %dx%d transform %d\n", d.name.c_str(), d.description.c_str(), d.x, d.y,
			            d.width, d.height, d.pixel_width, d.pixel_height, d.transform);
		return 0;
	}
	if (cmd == "multi") // multi SECONDS [portrait]: every output + one virtual output -> desktop_video_sender -> parser
	{
		QCoreApplication app(argc, argv);
		display_manager dm;
		if (!dm.is_kwin())
		{
			std::printf("kwin unavailable: %s\n", dm.kwin_unavailable_reason().toStdString().c_str());
			return 1;
		}
		dm.start({}, [](QString) {});
		QString err;
		auto vmode = dm.main_monitor_mode();
		std::printf("main mode %dx%d x%.2f @%d mHz\n", vmode.width, vmode.height, vmode.scale, vmode.refresh_mhz);
		if (argc > 4) { vmode.width = 1280; vmode.height = 720; vmode.refresh_mhz = std::stoi(argv[4]); }
		QObject::connect(&dm, &display_manager::log, [](QString l) { std::printf("%s\n", l.toStdString().c_str()); });
		auto vname = dm.add_virtual(vmode, &err);
		{ QEventLoop wait; QTimer::singleShot(3000, &wait, &QEventLoop::quit); wait.exec(); } // async mode change
		std::printf("virtual: %s %s\n", vname ? vname->c_str() : "FAILED", err.toStdString().c_str());
		auto list = dm.displays();
		if (argc > 3 && std::string(argv[3]) == "portrait" && vname)
			for (const auto & d: list)
				if (d.name == *vname)
				{
					bool ok = dm.set_portrait(d, true, &err);
					{ QEventLoop wait; QTimer::singleShot(3000, &wait, &QEventLoop::quit); wait.exec(); }
					std::printf("portrait: %s %s\n", ok ? "ok" : "FAILED", err.toStdString().c_str());
					list = dm.displays();
				}
		std::vector<desktop_stream_setup> setups;
		for (const auto & d: list)
		{
			std::printf("display %s%s logical %d,%d %dx%d @%d mHz\n", d.name.c_str(), d.is_virtual ? " (virtual)" : "", d.x, d.y, d.width, d.height, d.refresh_mhz);
			auto s = dm.open(d, &err);
			if (!s) { std::printf("open failed: %s\n", err.toStdString().c_str()); return 1; }
			s->bounds = {d.x, d.y, d.width, d.height};
			setups.push_back(*s);
		}
		int sv[2];
		socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
		std::map<int, int> frames, idrs;
		std::string formats;
		std::thread reader([&] {
			auto read_n = [&](void * p, size_t n) { size_t got = 0; while (got < n) { ssize_t r = read(sv[1], (char *)p + got, n - got); if (r <= 0) return false; got += size_t(r); } return true; };
			uint8_t type;
			while (read_n(&type, 1))
			{
				if (type == 4) { uint8_t f[112]; read_n(f, 112); int32_t v[28]; std::memcpy(v, f + 4, 108);
					formats += std::format("[codec {} fps {} count {} main {}x{} @{},{} {}x{} | 2nd {}x{} @{},{} {}x{}] ", v[0], v[1], v[4], v[2], v[3], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14]); }
				else if (type == 0) { uint8_t m[160]; read_n(m, 160); int32_t size; std::memcpy(&size, m + 152, 4); bytes au(static_cast<size_t>(size), 0); read_n(au.data(), au.size()); ++frames[0]; idrs[0] += m[13]; }
				else if (type == 1) { uint8_t h[16]; read_n(h, 16); int32_t idx, size; std::memcpy(&idx, h + 8, 4); std::memcpy(&size, h + 12, 4); bytes au(static_cast<size_t>(size), 0); read_n(au.data(), au.size()); ++frames[idx]; idrs[idx] += is_sync(au, video_codec::hevc); }
				else { std::printf("bad packet type %d\n", type); return; }
			}
		});
		{
			desktop_video_params p;
			p.codec = video_codec::hevc;
			p.kbps = 20000;
			stream_channel ch(sv[0], std::nullopt);
			desktop_video_sender sender(ch, p, setups, {[](std::string l) { std::fprintf(stderr, "%s\n", l.c_str()); }, [](video_codec) {}});
			QTimer::singleShot(std::stoi(argv[2]) * 1000 / 2, &app, [&] { sender.request_keyframe(); });
			QTimer::singleShot(std::stoi(argv[2]) * 1000, &app, &QCoreApplication::quit);
			app.exec();
			std::printf("running at end: %d\n", sender.running());
		}
		shutdown(sv[0], SHUT_RDWR);
		reader.join();
		std::printf("formats: %s\n", formats.c_str());
		for (auto [i, n]: frames)
			std::printf("stream %d: %d frames, %d IDR\n", i, n, idrs[i]);
		return 0;
	}
	if (cmd == "virtual-hold") // virtual-hold W H SECONDS: create a virtual output and keep it (for kscreen-doctor checks)
	{
		std::string why;
		auto k = kwin_screencast::connect(&why);
		if (!k) { std::printf("%s\n", why.c_str()); return 1; }
		stream_node node;
		std::string name, err;
		auto h = k->stream_virtual_output("VDTest", "vd test", std::stoi(argv[2]), std::stoi(argv[3]), 1.0, &node, &name, &err);
		std::printf("virtual %s node %u %s\n", name.c_str(), node.node_id, err.c_str());
		std::fflush(stdout);
		for (int i = 0; i < std::stoi(argv[4]) * 10; ++i) { k->pump(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
		return h ? 0 : 1;
	}
	if (cmd == "play") // play FILE SECONDS [SEEK_US]: file_player -> parser (video channel + audio channel)
	{
		int vs[2], as[2];
		socketpair(AF_UNIX, SOCK_STREAM, 0, vs);
		socketpair(AF_UNIX, SOCK_STREAM, 0, as);
		stream_channel vch(vs[0], std::nullopt), ach(as[0], std::nullopt);
		std::atomic<int> frames{0}, syncs{0}, subs{0}, formats{0};
		std::atomic<int64_t> audio_bytes{0}, first_ts{-1}, last_ts{0};
		std::thread vr([&] {
			auto rd = [&](void * p, size_t n) { size_t g = 0; while (g < n) { ssize_t r = read(vs[1], (char *)p + g, n - g); if (r <= 0) return false; g += size_t(r); } return true; };
			uint8_t t;
			while (rd(&t, 1))
			{
				if (t == 4) { uint8_t f[112]; rd(f, 112); int32_t v[28]; std::memcpy(v, f + 4, 108);
					std::printf("VideoFormat source %d codec %d fps %d %dx%d duration %.1f s\n", f[0], v[0], v[1], v[2], v[3], double((int64_t(v[5]) << 32) | uint32_t(v[6])) / 1e6); ++formats; }
				else if (t == 0) { uint8_t m[160]; rd(m, 160); int32_t size; int64_t ts; std::memcpy(&size, m + 152, 4); std::memcpy(&ts, m, 8); bytes au(static_cast<size_t>(size), 0); rd(au.data(), au.size());
					++frames; syncs += m[13]; if (first_ts < 0) first_ts = ts; last_ts = ts; }
				else if (t == 3) { int32_t n; rd(&n, 4); std::string txt(size_t(n), '\0'); rd(txt.data(), txt.size()); ++subs; std::printf("subtitle @%.2f: [%s]\n", last_ts / 1e6, txt.c_str()); }
				else { std::printf("bad packet %d\n", t); return; }
			}
		});
		std::thread ar([&] { uint8_t buf[65536]; ssize_t r; while ((r = read(as[1], buf, sizeof buf)) > 0) audio_bytes += r; });
		{
			file_player player(vch, &ach, argv[2], -1, 1.f, {[](std::string l) { std::fprintf(stderr, "%s\n", l.c_str()); }, [] { std::fprintf(stderr, "ended\n"); }});
			vch.send(player.video_format());
			for (const auto & t: player.audio_tracks()) std::printf("audio track %d '%s' %d ch\n", t.index, t.name.c_str(), t.channels);
			for (const auto & t: player.subtitle_tracks()) std::printf("subtitle track %d '%s' [%s]\n", t.index, t.name.c_str(), t.language.c_str());
			auto t0 = std::chrono::steady_clock::now();
			if (argc > 5 && !player.subtitle_tracks().empty()) player.set_subtitle_track(player.subtitle_tracks()[0].id);
			player.start(argc > 4 ? std::stoll(argv[4]) : 0);
			std::this_thread::sleep_for(std::chrono::seconds(std::stoi(argv[3])));
			double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			std::printf("after %.1f s: %d frames (%d sync), media %.2f..%.2f s, audio %.2f s, %d subtitle packets\n", el, int(frames), int(syncs),
			            first_ts / 1e6, last_ts / 1e6, audio_bytes / 192000.0, int(subs));
		}
		shutdown(vs[0], SHUT_RDWR); shutdown(as[0], SHUT_RDWR);
		vr.join(); ar.join();
		return 0;
	}
	if (cmd == "vr") // vr SECONDS OUT_LEFT OUT_RIGHT [codec h264|hevc]: VR sender with a turning fake head -> parser + eye dumps
	{
		int vs[2];
		socketpair(AF_UNIX, SOCK_STREAM, 0, vs);
		stream_channel ch(vs[0], std::nullopt);
		std::ofstream out_l(argv[3], std::ios::binary), out_r(argv[4], std::ios::binary);
		std::atomic<int> pairs{0}, mismatched{0}, syncs{0};
		std::atomic<int32_t> last_index{0};
		std::thread rd([&] {
			auto r = [&](void * p, size_t n) { size_t g = 0; while (g < n) { ssize_t k = read(vs[1], (char *)p + g, n - g); if (k <= 0) return false; g += size_t(k); } return true; };
			uint8_t t;
			int64_t frame_ts = -1;
			while (r(&t, 1))
			{
				if (t == 4) { uint8_t f[112]; r(f, 112); int32_t v[28]; std::memcpy(v, f + 4, 108);
					std::printf("VideoFormat source %d codec %d fps %d enc %dx%d streams %d bounds %dx%d second %dx%d\n", f[0], v[0], v[1], v[2], v[3], v[4], v[7], v[8], v[9], v[10]); }
				else if (t == 0) { uint8_t m[160]; r(m, 160); int32_t size, idx; std::memcpy(&size, m + 152, 4); std::memcpy(&frame_ts, m, 8); std::memcpy(&idx, m + 8, 4);
					bytes au(static_cast<size_t>(size), 0); r(au.data(), au.size()); out_l.write((char *)au.data(), size); syncs += m[13]; last_index = idx;
					if (m[12] != 1 || !m[28]) std::printf("bad VR metadata\n"); }
				else if (t == 1) { uint8_t h[16]; r(h, 16); int64_t ts; int32_t idx, size; std::memcpy(&ts, h, 8); std::memcpy(&idx, h + 8, 4); std::memcpy(&size, h + 12, 4);
					bytes au(static_cast<size_t>(size), 0); r(au.data(), au.size()); out_r.write((char *)au.data(), size); ++pairs; if (ts != frame_ts || idx != 1) ++mismatched; }
				else { std::printf("bad packet %d\n", t); return; }
			}
		});
		{
			vr_stream_params p;
			p.fps = 72;
			p.kbps = 60000;
			p.eye_width = 2112;
			p.eye_height = 2304;
			p.codec = argc > 5 && std::string(argv[5]) == "h264" ? video_codec::h264 : video_codec::hevc;
			auto t0 = std::chrono::steady_clock::now();
			vr_sender::callbacks cb;
			cb.log = [](std::string l) { std::fprintf(stderr, "%s\n", l.c_str()); };
			cb.pose = [t0] {
				vr_render_info info;
				float a = 0.5f * std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count(); // yaw, rad
				info.pose_index = int32_t(a * 1000);
				for (int e = 0; e < 2; ++e)
				{
					info.eye[e].orientation = {0, std::sin(a / 2), 0, std::cos(a / 2)};
					info.fov[e] = {0.9f, 1.0f, e == 0 ? 1.0f : 0.8f, e == 0 ? 0.8f : 1.0f};
				}
				return info;
			};
			vr_sender sender(ch, p, cb, true);
			std::this_thread::sleep_for(std::chrono::seconds(std::stoi(argv[2])));
			std::printf("running %d\n", sender.running());
		}
		shutdown(vs[0], SHUT_RDWR);
		rd.join();
		std::printf("%d stereo pairs, %d mismatched, %d sync, last pose index %d\n", int(pairs), int(mismatched), int(syncs), int(last_index));
		return 0;
	}
	if (cmd == "input-devices") // creates the uinput devices for a second, sends no events
	{
		input_injector inj;
		inj.set_layout({3440, 1440, {{{0, 0, 3440, 1440}, {}}}});
		std::this_thread::sleep_for(std::chrono::seconds(1));
		std::ifstream devices("/proc/bus/input/devices");
		std::string line, block;
		while (std::getline(devices, line))
		{
			if (line.empty())
			{
				if (block.find("OpenDesk") != std::string::npos)
					std::cout << block << "\n";
				block.clear();
			}
			else if (line.rfind("N:", 0) == 0 || line.rfind("H:", 0) == 0 || line.rfind("B: PROP", 0) == 0)
				block += line + "\n";
		}
		std::system("for e in $(grep -l 'OpenDesk' /sys/class/input/event*/device/name 2>/dev/null | xargs -n1 dirname | xargs -n1 dirname); do "
		            "printf '%s: ' \"$(cat $e/device/name)\"; udevadm info -q property /dev/input/$(basename $e) | grep -E '^ID_INPUT_(TOUCHSCREEN|MOUSE|KEYBOARD)=' | tr '\\n' ' '; echo; done");
		return 0;
	}
	if (cmd == "gamepad-ff") // gamepad-ff xbox|ds4 SECONDS: creates the pad (no input events) and prints rumble
	{
		auto model = std::string(argv[2]) == "ds4" ? gamepad_model::dualshock4 : gamepad_model::xbox360;
		virtual_gamepad pad(model, [](float strong, float weak) { std::printf("rumble strong=%.2f weak=%.2f\n", strong, weak); std::fflush(stdout); });
		std::printf("created\n");
		std::fflush(stdout);
		std::this_thread::sleep_for(std::chrono::seconds(std::stoi(argv[3])));
		return 0;
	}
	if (cmd == "encode") // encode W H FRAMES OUT: synthetic bgr0 frames through the memory path
	{
		int w = std::stoi(argv[2]), h = std::stoi(argv[3]), n = std::stoi(argv[4]);
		// optional: argv[6] codec id (1 h264, 2 hevc, 6 hevc10, 10 av1, 11 av1 10-bit), argv[7] "full" range
		video_codec codec = argc > 6 ? video_codec(std::stoi(argv[6])) : video_codec::h264;
		bool full = argc > 7 && std::string(argv[7]) == "full";
		std::string flags = argc > 8 ? argv[8] : ""; // "pre", "vbaq", "pre+vbaq"
		encoder_config ec{w, h, 60, 20000, codec, default_render_node(), full};
		ec.pre_encode = flags.find("pre") != std::string::npos;
		ec.vbaq = flags.find("vbaq") != std::string::npos;
		av_log_set_level(AV_LOG_WARNING);
		hardware_encoder enc(ec);
		std::ofstream out(argv[5], std::ios::binary);
		std::vector<uint8_t> img(size_t(w) * h * 4);
		int aus = 0, syncs = 0;
		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < n; ++i)
		{
			for (int y = 0; y < h; ++y) // moving gradient
				for (int x = 0; x < w; ++x)
				{
					uint8_t * p = &img[(size_t(y) * w + x) * 4];
					p[0] = uint8_t(x + i * 4); p[1] = uint8_t(y + i * 2); p[2] = uint8_t(i * 8); p[3] = 255;
				}
			captured_frame f;
			f.data = img.data();
			f.stride = uint32_t(w * 4);
			f.av_sw_format = vd_test_bgr0();
			enc.encode(f, i == n / 2, [&](bytes && au, bool sync) {
				out.write(reinterpret_cast<char *>(au.data()), std::streamsize(au.size()));
				++aus; syncs += sync;
			});
		}
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::printf("encoded %d AUs, %d IDR (expect 2: first + forced), %.2f ms/frame incl. CPU pattern, node %s\n", aus, syncs, ms / n,
		            enc.config().render_node.c_str());
		return 0;
	}
	if (cmd == "portal") // portal SECONDS OUT [TOKENFILE]: full portal -> PipeWire -> VAAPI path
	{
		QCoreApplication app(argc, argv);
		std::string token_file = argc > 4 ? argv[4] : "";
		QString token;
		if (!token_file.empty()) { std::ifstream t(token_file); std::string s; t >> s; token = QString::fromStdString(s); }
		std::ofstream out(argv[3], std::ios::binary);
		std::unique_ptr<pipewire_capture> cap;
		std::unique_ptr<hardware_encoder> enc;
		int frames = 0, syncs = 0;
		std::mutex m;
		screencast_portal portal;
		portal.start(token, false, [&](std::vector<portal_stream> list, QString new_token) {
			const portal_stream & s = list.front();
			std::cerr << "portal ok: node " << s.node_id << " " << s.width << "x" << s.height << (new_token.isEmpty() ? "" : " (token saved)") << "\n";
			if (!token_file.empty() && !new_token.isEmpty()) { std::ofstream t(token_file); t << new_token.toStdString(); }
			cap = std::make_unique<pipewire_capture>(portal.open_remote(), s.node_id, 0, 60,
			        [&](const captured_frame & f, const pipewire_capture::format & fmt) {
				        std::lock_guard l(m);
				        if (!enc) enc = std::make_unique<hardware_encoder>(encoder_config{fmt.width, fmt.height, 60, 30000, video_codec::h264, default_render_node()});
				        enc->encode(f, frames == 0, [&](bytes && au, bool sync) {
					        out.write(reinterpret_cast<char *>(au.data()), std::streamsize(au.size())); ++frames; syncs += sync; });
			        },
			        [](std::string l) { std::cerr << l << "\n"; });
			QTimer::singleShot(std::stoi(argv[2]) * 1000, &app, &QCoreApplication::quit);
		}, [&](QString e) { std::cerr << "portal error: " << e.toStdString() << "\n"; app.exit(1); });
		int rc = app.exec();
		cap.reset();
		std::printf("frames %d idr %d\n", frames, syncs);
		return rc;
	}
	return 2;
}

extern "C" {
#include <libavutil/pixfmt.h>
}
int vd_test_bgr0() { return AV_PIX_FMT_BGR0; }
