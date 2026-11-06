// Plays a video file to the headset (Windows: VideoStreamer + MediaReader), StreamingSource.Video:
//   video    : passed through untouched (the headset decodes H.264/HEVC/VP8/VP9/AV1 itself); H.264/HEVC converted to
//              Annex-B, AV1 given temporal delimiters. Frame packets with Source=Video, TimestampUs = pts / speed.
//              Other codecs (MPEG-2, MPEG-4 part 2 / DivX, VC-1 / WMV, ProRes, MJPEG, ...) are decoded and re-encoded to
//              H.264 on the fly (libx264 veryfast, no B-frames, SPS/PPS on every keyframe; a new encoder after a seek).
//   audio    : the chosen track decoded to s16 48 kHz stereo (atempo for the playback speed) on the audio channel
//   subtitles: text tracks decoded and sent as Subtitle packets (shown until the next one; "" clears)
//   VideoFormat: Source=Video, the file's codec/size/fps, duration in Bounds.X/Y (DurationUs = X << 32 | Y)
// Everything is paced in real time from the packets' timestamps; the headset shows frames at their timestamp.
#pragma once

#include "channel.h"
#include "video_source.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;

namespace od
{
class file_player : public video_source
{
public:
	struct audio_track
	{
		std::string name;
		int channels = 2;
		int index = -1; // stream index (SetAudioStreamIndex)
	};
	struct subtitle_track
	{
		std::array<uint8_t, 16> id{}; // SetSubtitleTrackId
		std::string name, language;
		int index = -1;
	};
	struct callbacks
	{
		std::function<void(std::string)> log; // any thread
		std::function<void()> ended;          // player thread: end of file reached (or a fatal error)
	};

	// throws if the file can't be opened or its video codec can't be passed to the headset
	file_player(stream_channel & video, stream_channel * audio, const std::string & path, int audio_index, float speed,
	            callbacks);
	~file_player() override;

	const std::vector<audio_track> & audio_tracks() const { return audio_list; }
	const std::vector<subtitle_track> & subtitle_tracks() const { return subtitle_list; }
	bytes video_format() const; // VideoFormat packet
	int64_t duration_us() const { return duration; }

	void start(int64_t start_us); // begins streaming (after the VideoFormat + track lists went out)
	void seek(int64_t us);
	void set_paused(bool);
	void set_speed(float);
	void set_audio_track(int stream_index);
	void set_subtitle_track(const std::array<uint8_t, 16> & id); // all zero = off
	void set_subtitle_offset_ms(int64_t ms);

	bool running() const override { return !finished; }
	void request_keyframe() override {} // passthrough: the next keyframe of the file comes when it comes
	void set_vibration(float, float) override {}

private:
	void run();
	struct state;
	std::unique_ptr<state> s;
	stream_channel & video_out;
	stream_channel * audio_out;
	callbacks cb;
	std::vector<audio_track> audio_list;
	std::vector<subtitle_track> subtitle_list;
	int64_t duration = 0;
	std::atomic<bool> finished{false};
	std::thread thread;
};
} // namespace od
