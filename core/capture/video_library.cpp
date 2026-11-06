#include "video_library.h"
#include "netbuf.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace od
{
namespace
{
enum file_id_options : int32_t // Upstream.Interfaces.FileIDOptions
{
	option_none = 0,
	option_folder = 1,
	option_hidden = 4,
};
constexpr size_t max_items = 500; // like the Windows streamer

int64_t dotnet_ticks(fs::file_time_type t)
{
	// .NET DateTime ticks: 100 ns since 0001-01-01; Unix epoch = 621355968000000000
	auto sys = std::chrono::file_clock::to_sys(t);
	auto ns100 = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(sys.time_since_epoch());
	return 621355968000000000LL + ns100.count();
}

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}
} // namespace

bool is_video_file(const std::string & name)
{
	static const std::set<std::string> exts{".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".wmv", ".flv", ".ts",
	                                        ".m2ts", ".mts", ".mpg", ".mpeg", ".3gp", ".ogv", ".vob"};
	return exts.contains(lower(fs::path(name).extension().string()));
}

std::optional<std::string> resolve_video_path(const std::string & root, const std::string & relative)
{
	if (root.empty())
		return std::nullopt;
	std::string rel = relative;
	std::replace(rel.begin(), rel.end(), '\\', '/');
	while (!rel.empty() && rel.front() == '/')
		rel.erase(rel.begin());
	std::error_code ec;
	fs::path base = fs::weakly_canonical(root, ec);
	fs::path full = fs::weakly_canonical(base / rel, ec);
	if (ec)
		return std::nullopt;
	auto [b, f] = std::mismatch(base.begin(), base.end(), full.begin(), full.end());
	if (b != base.end()) // outside the videos folder
		return std::nullopt;
	return full.string();
}

bytes list_videos(const std::string & root, const std::string & relative)
{
	struct item
	{
		std::string path;
		int64_t size, ticks;
		int32_t options;
	};
	std::vector<item> folders, files;
	auto dir = resolve_video_path(root, relative);
	std::error_code ec;
	if (dir && fs::is_directory(*dir, ec))
	{
		fs::path base = fs::weakly_canonical(root, ec);
		for (const auto & e: fs::directory_iterator(*dir, fs::directory_options::skip_permission_denied, ec))
		{
			std::string name = e.path().filename().string();
			std::string rel = "/" + fs::relative(e.path(), base, ec).generic_string();
			int32_t hidden = name.starts_with('.') ? option_hidden : option_none;
			auto time = e.last_write_time(ec);
			int64_t ticks = ec ? 0 : dotnet_ticks(time);
			if (e.is_directory(ec))
				folders.push_back({rel, 0, ticks, option_folder | hidden});
			else if (e.is_regular_file(ec) && is_video_file(name))
				files.push_back({rel, int64_t(e.file_size(ec)), ticks, hidden});
		}
	}
	auto by_path = [](const item & a, const item & b) { return lower(a.path) < lower(b.path); };
	std::sort(folders.begin(), folders.end(), by_path);
	std::sort(files.begin(), files.end(), by_path);
	folders.insert(folders.end(), files.begin(), files.end());
	if (folders.size() > max_items)
		folders.resize(max_items);
	net_writer w;
	w.i16(int16_t(folders.size()));
	for (const auto & i: folders)
		w.str(i.path).i64(i.size).i64(i.ticks).i32(i.options);
	return w.b;
}

std::optional<bytes> video_added(const std::string & root, const std::string & relative)
{
	auto file = resolve_video_path(root, relative);
	std::error_code ec;
	if (!file || !fs::is_regular_file(*file, ec))
		return std::nullopt;
	auto time = fs::last_write_time(*file, ec);
	net_writer w;
	w.str(relative).i64(int64_t(fs::file_size(*file, ec))).i64(ec ? 0 : dotnet_ticks(time)).i32(option_none);
	return w.b;
}

std::optional<bytes> video_thumbnail_png(const std::string & file, int width)
{
	AVFormatContext * fmt = nullptr;
	if (avformat_open_input(&fmt, file.c_str(), nullptr, nullptr) < 0)
		return std::nullopt;
	std::optional<bytes> result;
	AVCodecContext * dec = nullptr;
	AVFrame * frame = av_frame_alloc();
	AVPacket * pkt = av_packet_alloc();
	SwsContext * sws = nullptr;
	do
	{
		if (avformat_find_stream_info(fmt, nullptr) < 0)
			break;
		const AVCodec * codec = nullptr;
		int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
		if (si < 0 || !codec)
			break;
		AVStream * st = fmt->streams[si];
		dec = avcodec_alloc_context3(codec);
		avcodec_parameters_to_context(dec, st->codecpar);
		dec->thread_count = 0;
		if (avcodec_open2(dec, codec, nullptr) < 0)
			break;
		// a frame ~10% in (skips black intros), at most 5 minutes
		if (fmt->duration > 0)
		{
			int64_t target = std::min<int64_t>(fmt->duration / 10, 300LL * AV_TIME_BASE);
			av_seek_frame(fmt, -1, target, AVSEEK_FLAG_BACKWARD);
		}
		bool got = false;
		for (int packets = 0; !got && packets < 2000 && av_read_frame(fmt, pkt) >= 0; ++packets)
		{
			if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) >= 0)
				got = avcodec_receive_frame(dec, frame) >= 0;
			av_packet_unref(pkt);
		}
		if (!got)
		{
			avcodec_send_packet(dec, nullptr); // drain
			got = avcodec_receive_frame(dec, frame) >= 0;
		}
		if (!got || frame->width <= 0)
			break;
		int w = std::min(width, frame->width);
		int h = std::max(2, int(int64_t(frame->height) * w / frame->width) & ~1);
		sws = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR,
		                     nullptr, nullptr, nullptr);
		AVFrame * rgb = av_frame_alloc();
		rgb->format = AV_PIX_FMT_RGB24;
		rgb->width = w;
		rgb->height = h;
		av_frame_get_buffer(rgb, 0);
		sws_scale(sws, frame->data, frame->linesize, 0, frame->height, rgb->data, rgb->linesize);
		const AVCodec * png = avcodec_find_encoder(AV_CODEC_ID_PNG);
		AVCodecContext * enc = png ? avcodec_alloc_context3(png) : nullptr;
		if (enc)
		{
			enc->width = w;
			enc->height = h;
			enc->pix_fmt = AV_PIX_FMT_RGB24;
			enc->time_base = {1, 25};
			if (avcodec_open2(enc, png, nullptr) >= 0 && avcodec_send_frame(enc, rgb) >= 0 && avcodec_receive_packet(enc, pkt) >= 0)
			{
				result = bytes(pkt->data, pkt->data + pkt->size);
				av_packet_unref(pkt);
			}
			avcodec_free_context(&enc);
		}
		av_frame_free(&rgb);
	} while (false);
	sws_freeContext(sws);
	av_packet_free(&pkt);
	av_frame_free(&frame);
	avcodec_free_context(&dec);
	avformat_close_input(&fmt);
	return result;
}
} // namespace od
