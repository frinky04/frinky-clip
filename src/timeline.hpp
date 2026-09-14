#pragma once
#include "common.hpp"
#include <optional>

namespace clip {
// A closed buffer segment as the editor sees it: wall-clock extent in epoch
// milliseconds. Consecutive segments of a session meet exactly, so a run of
// footage is a chain of spans with start_ms equal to the previous end_ms.
struct Span {
    fs::path path;
    std::string session;
    std::int64_t start_ms = 0, end_ms = 0;
};
// The rolling buffer's closed segments in time order, plus the wall-clock end
// of the last closed segment (the open one continues from there).
struct BufferMap {
    std::vector<Span> spans;
    std::int64_t last_end_ms = 0;
};
BufferMap scan_buffer(const fs::path& buffer_root);
// The recorder publishes its in-memory segment list as one index file, so the
// editor need not read every sidecar. Returns nullopt when the index is
// missing or unreadable; callers fall back to scanning.
std::optional<BufferMap> read_index(const fs::path& index_path);
void write_index(const fs::path& index_path, const BufferMap& map);
// Segments of one session that overlap [start_ms, end_ms]. Empty when the
// range spans more than one session or touches no closed footage.
std::vector<Span> spans_in_range(const std::vector<Span>& spans, std::int64_t start_ms, std::int64_t end_ms);

struct ExportRequest {
    std::int64_t start_ms = 0, end_ms = 0;
    int height = 1080, fps = 60, bitrate_kbps = 20000;
    std::string codec = "h264"; // or "av1"
    bool audio = true;
};
ExportRequest read_export_request(const fs::path& path);
void write_export_request(const fs::path& path, const ExportRequest& request);
std::string clip_name(std::int64_t start_ms); // clip-YYYYMMDD-HHMMSS
std::string local_time(std::int64_t epoch_ms, bool with_ms = false); // HH:MM:SS[.mmm]
}
