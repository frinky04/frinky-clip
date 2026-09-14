#include "timeline.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>

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
            std::int64_t end = obs_data_get_int(d.get(), "end_ms"); double seconds = obs_data_get_double(d.get(), "seconds");
            if (end <= 0 || !std::isfinite(seconds) || seconds <= 0) continue;
            map.spans.push_back({media, name, end - (std::int64_t)std::llround(seconds * 1000), end});
        }
    }
    std::sort(map.spans.begin(), map.spans.end(), [](auto& a, auto& b) { return a.end_ms != b.end_ms ? a.end_ms < b.end_ms : a.path < b.path; });
    if (!map.spans.empty()) map.last_end_ms = map.spans.back().end_ms;
    return map;
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
static std::wstring seconds_text(std::int64_t ms) {
    wchar_t t[32]; swprintf_s(t, L"%lld.%03lld", ms / 1000, ms % 1000); return t;
}
std::vector<std::wstring> export_args(const ExportRequest& r, std::int64_t first_start_ms, const fs::path& list,
    const fs::path& progress, const fs::path& output) {
    // Output-side -ss/-t decode from the previous keyframe and drop frames before
    // the in-point, which is what makes the cut frame-accurate. fps= before
    // scale keeps frame selection on source frames.
    std::wstring encoder = r.codec == "av1" ? L"av1_nvenc" : L"h264_nvenc";
    std::wstring filter = L"fps=" + std::to_wstring(r.fps) + L",scale=-2:" + std::to_wstring(r.height);
    std::vector<std::wstring> args{L"-nostdin", L"-hide_banner", L"-y", L"-f", L"concat", L"-safe", L"0", L"-i", list.wstring(),
        L"-ss", seconds_text(std::max<std::int64_t>(0, r.start_ms - first_start_ms)), L"-t", seconds_text(std::max<std::int64_t>(1, r.end_ms - r.start_ms)),
        L"-map", L"0:v:0"};
    if (r.audio) args.insert(args.end(), {L"-map", L"0:a?"}); else args.push_back(L"-an");
    args.insert(args.end(), {L"-vf", filter, L"-c:v", encoder, L"-preset", L"p5", L"-rc", L"vbr", L"-b:v", std::to_wstring(r.bitrate_kbps) + L"k",
        L"-maxrate", std::to_wstring(r.bitrate_kbps * 3 / 2) + L"k", L"-bufsize", std::to_wstring(r.bitrate_kbps * 2) + L"k", L"-pix_fmt", L"yuv420p"});
    if (r.codec != "av1") args.insert(args.end(), {L"-profile:v", L"high"});
    if (r.audio) args.insert(args.end(), {L"-c:a", L"aac", L"-b:a", L"192k"});
    args.insert(args.end(), {L"-movflags", L"+faststart", L"-progress", progress.wstring(), L"-f", L"mp4", output.wstring()});
    return args;
}
std::optional<double> export_progress(const std::string& text, std::int64_t duration_ms) {
    // ffmpeg appends key=value blocks; the last out_time_us wins.
    std::optional<double> result; std::istringstream in(text); std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("out_time_us=")) {
            try { result = std::clamp(std::stod(line.substr(12)) / 1000.0 / std::max<std::int64_t>(1, duration_ms), 0.0, 1.0); } catch (...) {}
        } else if (line.starts_with("progress=end")) result = 1.0;
    }
    return result;
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
