#include "timeline.hpp"
#include "media.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>

namespace clip {
static bool reparse(const fs::path& path) { return (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }
BufferMap scan_buffer(const fs::path& root) {
    BufferMap map; std::error_code ec;
    if (!fs::is_directory(root, ec)) return map;
    for (auto& session : fs::directory_iterator(root, ec)) {
        if (!session.is_directory() || reparse(session.path()) || !session.path().filename().wstring().starts_with(L"session-")) continue;
        auto name = path_text(session.path().filename());
        for (auto& entry : fs::directory_iterator(session.path(), ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != L".json") continue;
            auto media = entry.path(); media.replace_extension(); // segment-x.mkv.json -> segment-x.mkv
            if (media.extension() != L".mkv" || !fs::exists(media, ec)) continue;
            auto d = read_json(entry.path());
            std::int64_t start = obs_data_get_int(d.get(), "start_ms"), end = obs_data_get_int(d.get(), "end_ms");
            if (start <= 0) { // Sidecar from before chained starts were recorded.
                double seconds = obs_data_get_double(d.get(), "seconds");
                if (!std::isfinite(seconds) || seconds <= 0) continue;
                start = end - (std::int64_t)std::llround(seconds * 1000);
            }
            if (end <= start) continue;
            Span span{media, name, start, end};
            span.coarse = downsample_levels(read_levels(d.get()), CoarseBinMs);
            map.spans.push_back(std::move(span));
        }
    }
    std::sort(map.spans.begin(), map.spans.end(), [](auto& a, auto& b) { return a.end_ms != b.end_ms ? a.end_ms < b.end_ms : a.path < b.path; });
    if (!map.spans.empty()) map.last_end_ms = map.spans.back().end_ms;
    return map;
}
std::optional<BufferMap> read_index(const fs::path& path) {
    auto text = read_text(path); if (text.empty()) return std::nullopt;
    auto d = Data(obs_data_create_from_json(text.c_str()), obs_data_release); if (!d) return std::nullopt;
    auto* array = obs_data_get_array(d.get(), "segments"); if (!array) return std::nullopt;
    BufferMap map; size_t count = obs_data_array_count(array);
    for (size_t i = 0; i < count; ++i) {
        auto item = Data(obs_data_array_item(array, i), obs_data_release);
        Span s{fs::path(wide(obs_data_get_string(item.get(), "path"))), obs_data_get_string(item.get(), "session"),
            obs_data_get_int(item.get(), "start_ms"), obs_data_get_int(item.get(), "end_ms")};
        s.coarse = read_levels(item.get());
        if (!s.path.empty() && s.end_ms > s.start_ms) map.spans.push_back(std::move(s));
    }
    obs_data_array_release(array);
    std::sort(map.spans.begin(), map.spans.end(), [](auto& a, auto& b) { return a.end_ms != b.end_ms ? a.end_ms < b.end_ms : a.path < b.path; });
    if (!map.spans.empty()) map.last_end_ms = map.spans.back().end_ms;
    return map;
}
void write_index(const fs::path& path, const BufferMap& map) {
    auto d = data(); auto* array = obs_data_array_create();
    for (auto& s : map.spans) {
        auto item = data(); obs_data_set_string(item.get(), "path", path_text(s.path).c_str()); obs_data_set_string(item.get(), "session", s.session.c_str());
        obs_data_set_int(item.get(), "start_ms", s.start_ms); obs_data_set_int(item.get(), "end_ms", s.end_ms);
        if (!s.coarse.empty()) write_levels(item.get(), s.coarse);
        obs_data_array_push_back(array, item.get());
    }
    obs_data_set_array(d.get(), "segments", array); obs_data_array_release(array);
    obs_data_set_int(d.get(), "updated_ms", now_ms()); // Index is transient: no flush.
    write_json_fast(path, d.get());
}
std::vector<Span> spans_in_range(const std::vector<Span>& spans, std::int64_t start_ms, std::int64_t end_ms) {
    std::vector<Span> result;
    for (auto& s : spans) if (s.end_ms > start_ms && s.start_ms < end_ms) result.push_back(s);
    for (auto& s : result) if (s.session != result.front().session) return {};
    return result;
}
ExportRequest read_export_request(const fs::path& path) {
    auto d = read_json(path); ExportRequest r;
    r.start_ms = obs_data_get_int(d.get(), "start_ms"); r.end_ms = obs_data_get_int(d.get(), "end_ms");
    r.height = (int)obs_data_get_int(d.get(), "height"); r.fps = (int)obs_data_get_int(d.get(), "fps");
    r.bitrate_kbps = (int)obs_data_get_int(d.get(), "bitrate_kbps"); r.codec = obs_data_get_string(d.get(), "codec");
    r.audio = obs_data_get_bool(d.get(), "audio");
    return r;
}
void write_export_request(const fs::path& path, const ExportRequest& r) {
    auto d = data(); obs_data_set_int(d.get(), "start_ms", r.start_ms); obs_data_set_int(d.get(), "end_ms", r.end_ms);
    obs_data_set_int(d.get(), "height", r.height); obs_data_set_int(d.get(), "fps", r.fps);
    obs_data_set_int(d.get(), "bitrate_kbps", r.bitrate_kbps); obs_data_set_string(d.get(), "codec", r.codec.c_str());
    obs_data_set_bool(d.get(), "audio", r.audio); write_json(path, d.get());
}
static std::tm local_tm(std::int64_t epoch_ms) {
    std::time_t t = epoch_ms / 1000; std::tm tm{}; localtime_s(&tm, &t); return tm;
}
std::string clip_name(std::int64_t start_ms) {
    auto tm = local_tm(start_ms); char t[48];
    snprintf(t, sizeof(t), "clip-%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return t;
}
std::string local_time(std::int64_t epoch_ms, bool with_ms) {
    auto tm = local_tm(epoch_ms); char t[32];
    if (with_ms) snprintf(t, sizeof(t), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(((epoch_ms % 1000) + 1000) % 1000));
    else snprintf(t, sizeof(t), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return t;
}
}
