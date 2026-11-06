// Videos tab (Windows: VideoStreamer's library part). The MEDIA tab's videos folder is the root; the headset
// browses it with relative '/' paths.
//   RequestVideos(path)            -> Videos: i16 count, {path, i64 size, i64 .NET ticks (UTC), i32 FileIDOptions}
//   RequestVideoThumbnails(paths)  -> VideoThumbnail(path, i32 size, PNG) per file
#pragma once

#include "bytes.h"

#include <optional>
#include <string>

namespace od
{
// absolute path of a headset path under root; nullopt if it escapes the root
std::optional<std::string> resolve_video_path(const std::string & root, const std::string & relative);
bool is_video_file(const std::string & name);
bytes list_videos(const std::string & root, const std::string & relative); // Videos payload
// VideoAdded payload (one FileID: path, i64 size, i64 ticks, i32 options) for a file under root; nullopt if missing
std::optional<bytes> video_added(const std::string & root, const std::string & relative);
std::optional<bytes> video_thumbnail_png(const std::string & file, int width = 320);
} // namespace od
