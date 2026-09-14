#pragma once
#include "common.hpp"
namespace clip {
struct MediaInfo { double seconds = 0; int width = 0, height = 0; bool audio = false; };
MediaInfo inspect_media(const fs::path& path);
void remux(const std::vector<fs::path>& segments, const fs::path& destination);
// A decoded picture scaled to `width` pixels, RGBA8, row-major.
struct Frame { int width = 0, height = 0; std::vector<std::uint8_t> rgba; std::int64_t pts_ms = 0; };
// Decode from `path` the last frame at or before `offset_ms` into the file.
// With `keyframe_only`, return the keyframe at or before it instead, which is
// much cheaper and what the timeline thumbnails use.
Frame decode_frame(const fs::path& path, std::int64_t offset_ms, int width, bool keyframe_only);
}
