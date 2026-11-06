#include "file_player.h"
#include "video.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <random>
#include <stdexcept>
#include <utility>
#include <algorithm>
#include <cmath>

namespace od
{
namespace
{
using clock = std::chrono::steady_clock;
constexpr uint8_t source_video = 2;       // StreamingSource.Video
constexpr uint8_t packet_subtitle = 3;    // PacketType.Subtitle
constexpr int64_t lead_us = 50000;        // video and audio go out this much ahead (same for both: stays in sync)
constexpr int64_t read_ahead_us = 1000000;

int64_t to_us(int64_t ts, AVRational tb)
{
	return ts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale_q(ts, tb, AVRational{1, 1000000});
}

std::string tag(AVDictionary * d, const char * key)
{
	AVDictionaryEntry * e = av_dict_get(d, key, nullptr, 0);
	return e && e->value ? e->value : "";
}

// ASS event ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text") -> plain text
std::string ass_text(const char * ass)
{
	std::string s = ass ? ass : "";
	size_t pos = 0;
	for (int commas = 0; commas < 8 && pos != std::string::npos; ++commas)
		pos = s.find(',', pos == 0 && commas == 0 ? 0 : pos + 1);
	std::string text = pos == std::string::npos ? s : s.substr(pos + 1);
	std::string out;
	for (size_t i = 0; i < text.size(); ++i)
	{
		if (text[i] == '{') // override block
		{
			size_t end = text.find('}', i);
			if (end == std::string::npos)
				break;
			i = end;
		}
		else if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == 'N' || text[i + 1] == 'n'))
			out += '\n', ++i;
		else if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'h')
			out += ' ', ++i;
		else
			out += text[i];
	}
	return out;
}

bool is_text_subtitle(AVCodecID id)
{
	switch (id)
	{
		case AV_CODEC_ID_SUBRIP:
		case AV_CODEC_ID_SRT:
		case AV_CODEC_ID_ASS:
		case AV_CODEC_ID_SSA:
		case AV_CODEC_ID_MOV_TEXT:
		case AV_CODEC_ID_WEBVTT:
		case AV_CODEC_ID_TEXT:
			return true;
		default:
			return false;
	}
}

// does an AV1 temporal unit carry a sequence header OBU?
bool av1_has_sequence_header(const uint8_t * p, size_t n)
{
	size_t i = 0;
	while (i < n)
	{
		uint8_t h = p[i];
		int type = (h >> 3) & 0x0f;
		bool ext = h & 0x04, has_size = h & 0x02;
		if (type == 1)
			return true;
		i += 1 + (ext ? 1 : 0);
		if (!has_size)
			return false;
		uint64_t size = 0;
		for (int k = 0; k < 8 && i < n; ++k)
		{
			uint8_t b = p[i++];
			size |= uint64_t(b & 0x7f) << (7 * k);
			if (!(b & 0x80))
				break;
		}
		i += size_t(size);
	}
	return false;
}
} // namespace

struct file_player::state
{
	AVFormatContext * fmt = nullptr;
	int vstream = -1;
	AVBSFContext * bsf = nullptr;
	// a codec the headset can't decode (MPEG-2, MPEG-4 part 2, VC-1, ProRes, ...): decoded here, re-encoded to H.264
	bool transcode = false;
	AVCodecContext * vdec = nullptr, * venc = nullptr;
	SwsContext * sws = nullptr;
	AVFrame * yuv = nullptr;
	AVRational vtime_base{1, 1000};
	video_codec wire_codec{};
	int32_t wire_codec_value = 0;
	int width = 0, height = 0, fps = 30;
	bool hdr = false;
	bytes av1_config; // sequence header OBU(s) from av1C
	int64_t start_offset = 0; // stream start time (us)

	// audio
	int astream = -1;
	AVCodecContext * adec = nullptr;
	AVFilterGraph * agraph = nullptr;
	AVFilterContext * asrc = nullptr;
	AVFilterContext * asink = nullptr;
	float agraph_speed = 0;

	// subtitles
	int sstream = -1;
	AVCodecContext * sdec = nullptr;
	struct cue
	{
		int64_t show_us, hide_us;
		std::string text;
	};
	std::vector<cue> cues;
	std::string shown;

	// control (guarded by m)
	std::mutex m;
	std::condition_variable cv;
	bool quit = false, paused = false, changed = false;
	int64_t seek_to = -1;
	float speed = 1;
	int audio_request = -2;  // -2: no change
	int subtitle_request = -2;
	int64_t subtitle_offset_us = 0;

	// clock: media time (us) <-> wall time
	float cur_speed = 1;
	bool rebase = true;
	clock::time_point base_wall;
	int64_t base_media = 0;
	int64_t media_now() const
	{
		return base_media + int64_t(double(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - base_wall).count()) * cur_speed);
	}
	clock::time_point wall_of(int64_t media) const
	{
		return base_wall + std::chrono::microseconds(int64_t(double(media - base_media) / cur_speed));
	}

	std::multimap<int64_t, AVPacket *> queue; // by send time (media us)
	bool eof = false;
	int64_t last_read_us = 0;
	int32_t frame_index = 0;

	~state()
	{
		for (auto & [k, p]: queue)
			av_packet_free(&p);
		avfilter_graph_free(&agraph);
		avcodec_free_context(&adec);
		avcodec_free_context(&sdec);
		avcodec_free_context(&vdec);
		avcodec_free_context(&venc);
		sws_freeContext(sws);
		av_frame_free(&yuv);
		av_bsf_free(&bsf);
		avformat_close_input(&fmt);
	}
};

file_player::file_player(stream_channel & video, stream_channel * audio, const std::string & path, int audio_index, float speed,
                         callbacks c) :
        s(std::make_unique<state>()), video_out(video), audio_out(audio), cb(std::move(c))
{
	if (avformat_open_input(&s->fmt, path.c_str(), nullptr, nullptr) < 0)
		throw std::runtime_error("cannot open " + path);
	if (avformat_find_stream_info(s->fmt, nullptr) < 0)
		throw std::runtime_error("cannot read the streams of " + path);
	s->vstream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (s->vstream < 0)
		throw std::runtime_error("no video stream");
	AVStream * vs = s->fmt->streams[s->vstream];
	AVCodecParameters * par = vs->codecpar;
	const char * bsf_name = nullptr;
	switch (par->codec_id)
	{
		case AV_CODEC_ID_H264: s->wire_codec_value = 1; bsf_name = "h264_mp4toannexb"; break;
		case AV_CODEC_ID_HEVC: s->wire_codec_value = 2; bsf_name = "hevc_mp4toannexb"; break;
		case AV_CODEC_ID_VP8: s->wire_codec_value = 3; break;
		case AV_CODEC_ID_VP9: s->wire_codec_value = 4; break;
		case AV_CODEC_ID_AV1: s->wire_codec_value = 10; break;
		default:
		{
			// the headset decodes none of these: decode here, stream H.264 (libx264)
			const AVCodec * dec = avcodec_find_decoder(par->codec_id);
			if (!dec || !avcodec_find_encoder_by_name("libx264"))
				throw std::runtime_error(std::format("{} video can't be played on the headset (no {})", avcodec_get_name(par->codec_id),
				                                     dec ? "libx264 encoder to convert it" : "decoder"));
			s->vdec = avcodec_alloc_context3(dec);
			avcodec_parameters_to_context(s->vdec, par);
			s->vdec->pkt_timebase = vs->time_base;
			s->vdec->thread_count = 0;
			s->vdec->thread_type = FF_THREAD_SLICE; // frame threads would hold back a frame per thread (start, every seek)
			if (avcodec_open2(s->vdec, dec, nullptr) < 0)
				throw std::runtime_error(std::format("cannot decode {} video", avcodec_get_name(par->codec_id)));
			s->transcode = true;
			s->wire_codec_value = 1;
			s->vtime_base = vs->time_base;
			break;
		}
	}
	if (bsf_name)
	{
		av_bsf_alloc(av_bsf_get_by_name(bsf_name), &s->bsf);
		avcodec_parameters_copy(s->bsf->par_in, par);
		s->bsf->time_base_in = vs->time_base;
		if (av_bsf_init(s->bsf) < 0)
			throw std::runtime_error("bitstream filter setup failed");
	}
	if (par->codec_id == AV_CODEC_ID_AV1 && par->extradata_size > 4 && (par->extradata[0] & 0x80)) // av1C + configOBUs
		s->av1_config.assign(par->extradata + 4, par->extradata + par->extradata_size);
	s->width = par->width;
	s->height = par->height;
	AVRational fr = av_guess_frame_rate(s->fmt, vs, nullptr);
	s->fps = fr.num > 0 && fr.den > 0 ? int(std::lround(av_q2d(fr))) : 30;
	s->hdr = !s->transcode && (par->color_trc == AVCOL_TRC_SMPTE2084 || par->color_trc == AVCOL_TRC_ARIB_STD_B67);
	s->start_offset = vs->start_time != AV_NOPTS_VALUE ? to_us(vs->start_time, vs->time_base) : 0;
	duration = s->fmt->duration > 0 ? s->fmt->duration : 0;
	s->speed = s->cur_speed = speed > 0 ? speed : 1;

	std::mt19937_64 rng{std::random_device{}()};
	for (unsigned i = 0; i < s->fmt->nb_streams; ++i)
	{
		AVStream * st = s->fmt->streams[i];
		std::string title = tag(st->metadata, "title"), lang = tag(st->metadata, "language");
		if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			audio_track t;
			t.index = int(i);
			t.channels = st->codecpar->ch_layout.nb_channels;
			t.name = !title.empty() ? title : !lang.empty() ? lang : std::format("Track {}", audio_list.size() + 1);
			audio_list.push_back(t);
		}
		else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE && is_text_subtitle(st->codecpar->codec_id))
		{
			subtitle_track t;
			t.index = int(i);
			for (auto & b: t.id)
				b = uint8_t(rng());
			t.name = !title.empty() ? title : !lang.empty() ? lang : std::format("Subtitles {}", subtitle_list.size() + 1);
			t.language = lang;
			subtitle_list.push_back(t);
		}
	}
	int a = audio_index >= 0 ? audio_index : av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, s->vstream, nullptr, 0);
	s->audio_request = a >= 0 ? a : -1;
	for (unsigned i = 0; i < s->fmt->nb_streams; ++i) // don't demux what we don't use
		s->fmt->streams[i]->discard = (int(i) == s->vstream || s->fmt->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
		                                      ? AVDISCARD_DEFAULT
		                                      : AVDISCARD_ALL;
	if (cb.log)
		cb.log(std::format("[videos] {}: {} {}x{} @{} fps{}{}, {:.0f} s, {} audio / {} subtitle tracks", path,
		                   avcodec_get_name(par->codec_id), s->width, s->height, s->fps, s->hdr ? " HDR" : "",
		                   s->transcode ? " (converted to H.264 for the headset)" : "", duration / 1e6, audio_list.size(),
		                   subtitle_list.size()));
}

file_player::~file_player()
{
	{
		std::lock_guard lock(s->m);
		s->quit = true;
	}
	s->cv.notify_all();
	if (thread.joinable())
		thread.join();
}

bytes file_player::video_format() const
{
	std::vector<stream_format> f{{s->width, s->height, {int(duration >> 32), int(duration & 0xffffffff), 0, 0}}};
	return pack_video_format(source_video, video_codec(s->wire_codec_value), s->fps, f);
}

void file_player::start(int64_t start_us)
{
	if (start_us > 0)
		s->seek_to = start_us;
	thread = std::thread(&file_player::run, this);
}

void file_player::seek(int64_t us)
{
	{
		std::lock_guard lock(s->m);
		s->seek_to = std::max<int64_t>(us, 0);
		s->changed = true;
	}
	s->cv.notify_all();
}

void file_player::set_paused(bool p)
{
	{
		std::lock_guard lock(s->m);
		s->paused = p;
		s->changed = true;
	}
	s->cv.notify_all();
}

void file_player::set_speed(float sp)
{
	{
		std::lock_guard lock(s->m);
		s->speed = sp > 0 ? sp : 1;
		s->changed = true;
	}
	s->cv.notify_all();
}

void file_player::set_audio_track(int index)
{
	{
		std::lock_guard lock(s->m);
		s->audio_request = index;
		s->changed = true;
	}
	s->cv.notify_all();
}

void file_player::set_subtitle_track(const std::array<uint8_t, 16> & id)
{
	int index = -1;
	for (const auto & t: subtitle_list)
		if (t.id == id)
			index = t.index;
	{
		std::lock_guard lock(s->m);
		s->subtitle_request = index;
		s->changed = true;
	}
	s->cv.notify_all();
}

void file_player::set_subtitle_offset_ms(int64_t ms)
{
	std::lock_guard lock(s->m);
	s->subtitle_offset_us = ms * 1000;
}

void file_player::run()
{
	state & st = *s;
	AVPacket * pkt = av_packet_alloc();
	auto send_subtitle = [&](const std::string & text) {
		if (text == st.shown)
			return;
		st.shown = text;
		bytes b{packet_subtitle};
		append_le(b, int32_t(text.size()));
		b.insert(b.end(), text.begin(), text.end());
		video_out.send(b);
	};
	auto open_decoder = [&](int index, AVCodecContext *& ctx) {
		avcodec_free_context(&ctx);
		if (index < 0 || index >= int(st.fmt->nb_streams))
			return;
		AVStream * as = st.fmt->streams[index];
		const AVCodec * codec = avcodec_find_decoder(as->codecpar->codec_id);
		if (!codec)
			return;
		ctx = avcodec_alloc_context3(codec);
		avcodec_parameters_to_context(ctx, as->codecpar);
		ctx->pkt_timebase = as->time_base;
		if (avcodec_open2(ctx, codec, nullptr) < 0)
			avcodec_free_context(&ctx);
	};
	// audio: decoder -> aresample 48 kHz -> atempo (speed) -> s16 stereo
	auto build_audio_graph = [&] {
		avfilter_graph_free(&st.agraph);
		st.asrc = st.asink = nullptr;
		if (!st.adec)
			return;
		st.agraph = avfilter_graph_alloc();
		char layout[64] = "stereo";
		av_channel_layout_describe(&st.adec->ch_layout, layout, sizeof(layout));
		std::string args = std::format("time_base=1/{}:sample_rate={}:sample_fmt={}:channel_layout={}", st.adec->sample_rate,
		                               st.adec->sample_rate, av_get_sample_fmt_name(st.adec->sample_fmt), layout);
		avfilter_graph_create_filter(&st.asrc, avfilter_get_by_name("abuffer"), "in", args.c_str(), nullptr, st.agraph);
		avfilter_graph_create_filter(&st.asink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, st.agraph);
		std::string chain = "aresample=48000";
		for (double sp = st.cur_speed; std::abs(sp - 1) > 0.001;) // atempo takes 0.5..2 per instance
		{
			double step = std::clamp(sp, 0.5, 2.0);
			chain += std::format(",atempo={}", step);
			sp /= step;
		}
		chain += ",aformat=sample_fmts=s16:sample_rates=48000:channel_layouts=stereo";
		AVFilterInOut * outputs = avfilter_inout_alloc();
		AVFilterInOut * inputs = avfilter_inout_alloc();
		outputs->name = av_strdup("in");
		outputs->filter_ctx = st.asrc;
		inputs->name = av_strdup("out");
		inputs->filter_ctx = st.asink;
		if (avfilter_graph_parse_ptr(st.agraph, chain.c_str(), &inputs, &outputs, nullptr) < 0 || avfilter_graph_config(st.agraph, nullptr) < 0)
		{
			if (cb.log)
				cb.log("[videos] audio filter setup failed; playing without sound");
			avfilter_graph_free(&st.agraph);
			st.asrc = st.asink = nullptr;
		}
		avfilter_inout_free(&inputs);
		avfilter_inout_free(&outputs);
		st.agraph_speed = st.cur_speed;
	};
	auto pull_audio = [&] {
		if (!st.asink)
			return;
		AVFrame * f = av_frame_alloc();
		while (av_buffersink_get_frame(st.asink, f) >= 0)
		{
			size_t bytes_total = size_t(f->nb_samples) * 4;
			for (size_t off = 0; off < bytes_total; off += 3840) // <= 20 ms per packet
			{
				size_t n = std::min<size_t>(3840, bytes_total - off);
				bytes b;
				b.reserve(4 + n);
				append_le(b, int32_t(n));
				b.insert(b.end(), f->data[0] + off, f->data[0] + off + n);
				if (audio_out)
					audio_out->send(b);
			}
			av_frame_unref(f);
		}
		av_frame_free(&f);
	};
	auto set_stream_discard = [&] {
		for (unsigned i = 0; i < st.fmt->nb_streams; ++i)
			st.fmt->streams[i]->discard = (int(i) == st.vstream || int(i) == st.astream || int(i) == st.sstream) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
	};
	auto clear_queue = [&] {
		for (auto & [k, p]: st.queue)
			av_packet_free(&p);
		st.queue.clear();
	};
	auto do_seek = [&](int64_t target) {
		clear_queue();
		av_seek_frame(st.fmt, -1, target + st.start_offset, AVSEEK_FLAG_BACKWARD);
		if (st.bsf)
			av_bsf_flush(st.bsf);
		if (st.vdec)
			avcodec_flush_buffers(st.vdec);
		avcodec_free_context(&st.venc); // a new encoder: the stream restarts with a keyframe
		if (st.adec)
			avcodec_flush_buffers(st.adec);
		if (st.sdec)
			avcodec_flush_buffers(st.sdec);
		build_audio_graph(); // drop buffered samples
		st.cues.clear();
		send_subtitle("");
		st.eof = false;
		st.rebase = true;
	};

	bool ok = true;
	while (ok)
	{
		// --- control
		{
			std::unique_lock lock(st.m);
			if (st.quit)
				break;
			if (st.paused)
			{
				int64_t at = st.rebase ? st.base_media : st.media_now();
				st.cv.wait(lock, [&] { return st.quit || !st.paused || st.seek_to >= 0; });
				if (st.quit)
					break;
				st.base_media = at;
				st.base_wall = clock::now();
			}
			int64_t seek = std::exchange(st.seek_to, -1);
			int audio_req = std::exchange(st.audio_request, -2);
			int sub_req = std::exchange(st.subtitle_request, -2);
			float speed = st.speed;
			st.changed = false;
			lock.unlock();
			if (speed != st.cur_speed)
			{
				int64_t now_media = st.rebase ? st.base_media : st.media_now();
				st.cur_speed = speed;
				st.base_media = now_media;
				st.base_wall = clock::now();
				build_audio_graph();
			}
			if (audio_req != -2)
			{
				st.astream = audio_req;
				open_decoder(st.astream, st.adec);
				build_audio_graph();
				set_stream_discard();
			}
			if (sub_req != -2)
			{
				st.sstream = sub_req;
				open_decoder(st.sstream, st.sdec);
				st.cues.clear();
				send_subtitle("");
				set_stream_discard();
			}
			if (seek >= 0 || audio_req != -2 || sub_req != -2)
				do_seek(seek >= 0 ? seek : (st.rebase ? st.base_media : st.media_now()));
		}

		// --- read ahead
		int64_t horizon = (st.rebase ? st.last_read_us : st.media_now()) + read_ahead_us;
		while (!st.eof && (st.queue.empty() || st.last_read_us < horizon))
		{
			int r = av_read_frame(st.fmt, pkt);
			if (r < 0)
			{
				st.eof = true;
				break;
			}
			int si = pkt->stream_index;
			AVStream * as = st.fmt->streams[si];
			int64_t ts = to_us(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts, as->time_base);
			if ((si != st.vstream && si != st.astream && si != st.sstream) || ts == AV_NOPTS_VALUE)
			{
				av_packet_unref(pkt);
				continue;
			}
			ts -= st.start_offset;
			if (si == st.sstream && st.sdec) // subtitles: decode now, schedule by time
			{
				AVSubtitle sub{};
				int got = 0;
				if (avcodec_decode_subtitle2(st.sdec, &sub, &got, pkt) >= 0 && got)
				{
					std::string text;
					for (unsigned r2 = 0; r2 < sub.num_rects; ++r2)
					{
						AVSubtitleRect * rect = sub.rects[r2];
						std::string line = rect->type == SUBTITLE_ASS ? ass_text(rect->ass) : rect->text ? rect->text : "";
						if (!line.empty())
							text += (text.empty() ? "" : "\n") + line;
					}
					int64_t show = ts + int64_t(sub.start_display_time) * 1000;
					int64_t hide = sub.end_display_time && sub.end_display_time != UINT32_MAX
					                       ? ts + int64_t(sub.end_display_time) * 1000
					                       : ts + std::max<int64_t>(to_us(pkt->duration, as->time_base), 3000000);
					st.cues.push_back({show, hide, text});
					avsubtitle_free(&sub);
				}
				av_packet_unref(pkt);
				continue;
			}
			st.last_read_us = std::max(st.last_read_us, ts);
			AVPacket * q = av_packet_clone(pkt);
			av_packet_unref(pkt);
			st.queue.emplace(ts - lead_us, q);
		}

		if (st.queue.empty())
		{
			if (!st.eof)
				continue;
			// end of file: let the last frames play out, then report
			std::unique_lock lock(st.m);
			if (!st.rebase)
				st.cv.wait_until(lock, st.wall_of(st.last_read_us + lead_us + 500000), [&] { return st.quit || st.changed; });
			if (st.quit || st.changed)
				continue;
			break;
		}

		// --- next packet in time order
		auto it = st.queue.begin();
		if (st.rebase)
		{
			st.base_media = it->first;
			st.base_wall = clock::now();
			st.rebase = false;
		}
		// subtitle cues due before this packet
		int64_t next_media = it->first;
		{
			std::lock_guard lock(st.m);
			int64_t offset = st.subtitle_offset_us;
			for (auto c = st.cues.begin(); c != st.cues.end();)
			{
				if (c->hide_us + offset <= st.media_now())
				{
					if (st.shown == c->text)
						send_subtitle("");
					c = st.cues.erase(c);
					continue;
				}
				if (c->show_us + offset <= st.media_now())
					send_subtitle(c->text);
				next_media = std::min({next_media, c->show_us + offset > st.media_now() ? c->show_us + offset : next_media,
				                       c->hide_us + offset});
				++c;
			}
		}
		{
			std::unique_lock lock(st.m);
			if (st.cv.wait_until(lock, st.wall_of(next_media), [&] { return st.quit || st.changed; }))
				continue; // control change: re-evaluate
		}
		if (next_media < it->first)
			continue; // a subtitle cue came first
		AVPacket * p = it->second;
		st.queue.erase(it);
		int64_t ts = to_us(p->pts != AV_NOPTS_VALUE ? p->pts : p->dts, st.fmt->streams[p->stream_index]->time_base) - st.start_offset;

		if (p->stream_index == st.vstream)
		{
			bool key = p->flags & AV_PKT_FLAG_KEY;
			auto send_video = [&](const uint8_t * data, size_t size, bool sync, int64_t ts) {
				bytes au;
				if (st.wire_codec_value == 10) // AV1: temporal delimiter (+ sequence header on keyframes if missing)
				{
					au = {0x12, 0x00};
					if (sync && !st.av1_config.empty() && !av1_has_sequence_header(data, size))
						au.insert(au.end(), st.av1_config.begin(), st.av1_config.end());
				}
				au.insert(au.end(), data, data + size);
				bytes packet = pack_frame_header(int64_t(double(ts) / st.cur_speed), st.frame_index++, source_video, sync, st.width,
				                                 st.height, int32_t(au.size()), 1000.f / float(st.fps));
				packet[1 + 14] = st.hdr; // FrameMetadata.IsHDR
				packet.insert(packet.end(), au.begin(), au.end());
				if (!video_out.send(packet))
					ok = false;
			};
			if (st.transcode)
			{
				AVFrame * f = av_frame_alloc();
				if (avcodec_send_packet(st.vdec, p) >= 0)
					while (ok && avcodec_receive_frame(st.vdec, f) >= 0)
					{
						if (!st.venc) // after the first frame (its size / format) and after every seek
						{
							const AVCodec * enc = avcodec_find_encoder_by_name("libx264");
							st.venc = avcodec_alloc_context3(enc);
							st.venc->width = f->width;
							st.venc->height = f->height;
							st.venc->pix_fmt = AV_PIX_FMT_YUV420P;
							st.venc->time_base = st.vtime_base;
							st.venc->framerate = {st.fps, 1};
							st.venc->gop_size = st.fps * 2;
							st.venc->thread_count = 0;
							av_opt_set(st.venc->priv_data, "preset", "veryfast", 0);
							av_opt_set(st.venc->priv_data, "tune", "zerolatency", 0); // no B-frames, no delay
							av_opt_set(st.venc->priv_data, "crf", "19", 0);
							av_opt_set(st.venc->priv_data, "x264-params", "repeat-headers=1", 0); // SPS/PPS on every keyframe
							if (avcodec_open2(st.venc, enc, nullptr) < 0)
							{
								avcodec_free_context(&st.venc);
								ok = false;
								break;
							}
							av_frame_free(&st.yuv);
							st.yuv = av_frame_alloc();
							st.yuv->format = AV_PIX_FMT_YUV420P;
							st.yuv->width = f->width;
							st.yuv->height = f->height;
							av_frame_get_buffer(st.yuv, 0);
						}
						st.sws = sws_getCachedContext(st.sws, f->width, f->height, AVPixelFormat(f->format), f->width, f->height,
						                              AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
						av_frame_make_writable(st.yuv);
						sws_scale(st.sws, f->data, f->linesize, 0, f->height, st.yuv->data, st.yuv->linesize);
						st.yuv->pts = f->best_effort_timestamp;
						if (avcodec_send_frame(st.venc, st.yuv) >= 0)
							while (avcodec_receive_packet(st.venc, pkt) >= 0)
							{
								int64_t out_ts = pkt->pts != AV_NOPTS_VALUE ? to_us(pkt->pts, st.vtime_base) - st.start_offset : ts;
								send_video(pkt->data, size_t(pkt->size), pkt->flags & AV_PKT_FLAG_KEY, out_ts);
								av_packet_unref(pkt);
							}
						av_frame_unref(f);
					}
				av_frame_free(&f);
			}
			else if (st.bsf)
			{
				if (av_bsf_send_packet(st.bsf, p) >= 0)
					while (av_bsf_receive_packet(st.bsf, pkt) >= 0)
					{
						send_video(pkt->data, size_t(pkt->size), pkt->flags & AV_PKT_FLAG_KEY, ts);
						av_packet_unref(pkt);
					}
			}
			else
				send_video(p->data, size_t(p->size), key, ts);
		}
		else if (p->stream_index == st.astream && st.adec)
		{
			if (avcodec_send_packet(st.adec, p) >= 0)
			{
				AVFrame * f = av_frame_alloc();
				while (avcodec_receive_frame(st.adec, f) >= 0)
				{
					if (st.asrc)
						av_buffersrc_add_frame(st.asrc, f);
					av_frame_unref(f);
				}
				av_frame_free(&f);
				pull_audio();
			}
		}
		av_packet_free(&p);
	}
	av_packet_free(&pkt);
	send_subtitle("");
	finished = true;
	bool quitting;
	{
		std::lock_guard lock(st.m);
		quitting = st.quit;
	}
	if (!quitting && cb.ended)
		cb.ended();
}
} // namespace od
