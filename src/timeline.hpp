#pragma once
#include "common.hpp"
#include <optional>

namespace clip {
// A closed buffer segment as the editor sees it: wall-clock extent in epoch
// milliseconds, read from the committed sidecar next to each MKV.
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
// ffmpeg arguments for a frame-accurate re-encode of `request` from `sources`
// listed in `concat_list`, starting `first_start_ms` at the concatenation's
// zero. Progress is written to `progress_file`; output goes to `output`.
std::vector<std::wstring> export_args(const ExportRequest& request, std::int64_t first_start_ms, const fs::path& concat_list,
    const fs::path& progress_file, const fs::path& output);
// Fraction complete from an ffmpeg -progress file, or nullopt before any report.
std::optional<double> export_progress(const std::string& progress_text, std::int64_t duration_ms);
std::string clip_name(std::int64_t start_ms); // clip-YYYYMMDD-HHMMSS
std::string local_time(std::int64_t epoch_ms, bool with_ms = false); // HH:MM:SS[.mmm]
}
