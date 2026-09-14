#pragma once
#include "common.hpp"
namespace clip {
struct MediaInfo { double seconds = 0; int width = 0, height = 0; bool audio = false; };
MediaInfo inspect_media(const fs::path& path);
void remux(const std::vector<fs::path>& segments, const fs::path& destination);
}
