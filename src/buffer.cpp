#include "buffer.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace clip {
std::vector<size_t> select_recent(const std::vector<Segment>& segments, double seconds, const std::string& session) {
    std::vector<size_t> result; double remaining = seconds;
    // A clip must stay within one encoder session (settings may have changed).
    auto wanted = session.empty() && !segments.empty() ? segments.back().session : session;
    for (size_t i = segments.size(); i > 0 && remaining > 0; --i) {
        if (segments[i-1].session != wanted) continue;
        result.push_back(i-1); remaining -= segments[i-1].seconds();
    }
    std::reverse(result.begin(), result.end()); return result;
}
std::vector<size_t> expired_segments(const std::vector<Segment>& segments, std::int64_t now, double retention_seconds,
    std::uintmax_t limit, const std::set<fs::path>& pinned) {
    std::uintmax_t total = 0; for (auto& s : segments) total += s.bytes;
    std::vector<size_t> result;
    for (size_t i = 0; i < segments.size(); ++i) {
        auto& s = segments[i];
        if (pinned.contains(s.path)) continue;
        if (s.end_ms < now - retention_seconds * 1000 || total > limit) { result.push_back(i); total -= s.bytes; }
    }
    return result;
}
std::int64_t chained_ms(std::int64_t anchor_ms, std::int64_t frames, int fps_num, int fps_den) {
    // Rounded once from the exact frame count, so rounding never accumulates.
    return anchor_ms + (std::int64_t)std::llround((double)frames * 1000.0 * fps_den / std::max(1, fps_num));
}
Buffer::Buffer(const Config& c) : config_(c), root_(c.storage / "buffer") {
    if (fs::exists(root_) && !fs::exists(root_ / ".frinky-buffer") && !fs::is_empty(root_))
        throw std::runtime_error("Buffer directory contains unrelated files. Choose another storage folder.");
    fs::create_directories(root_);
    if (!fs::exists(root_ / ".frinky-buffer")) atomic_write(root_ / ".frinky-buffer", "Frinky Clip buffer v1\n");
    fs::create_directories(config_.storage / "clips"); fs::create_directories(config_.storage / "pending");
}
static bool reparse(const fs::path& path) { return (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }
static std::int64_t write_time_ms(const fs::path& p) {
    auto stamp = fs::last_write_time(p);
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::clock_cast<std::chrono::system_clock>(stamp).time_since_epoch()).count();
}
void Buffer::sort() {
    std::sort(segments_.begin(), segments_.end(), [](auto& a, auto& b) { return a.end_ms != b.end_ms ? a.end_ms < b.end_ms : a.path < b.path; });
}
void Buffer::anchor(const std::string& session, std::int64_t start_ms) { chains_[session] = Chain{start_ms, 0}; }
bool Buffer::finalize(const fs::path& p) {
    if (!fs::exists(p) || !flush_closed(p)) return false;
    if (std::any_of(segments_.begin(), segments_.end(), [&](auto& s) { return s.path == p; })) return true;
    auto session = path_text(p.parent_path().filename());
    auto metadata = p; metadata += L".json";
    auto d = read_json(metadata);
    std::int64_t start = obs_data_get_int(d.get(), "start_ms"), end = obs_data_get_int(d.get(), "end_ms");
    if (!(start > 0 && end > start)) {
        VideoExtent video = probe_video(p);
        auto& chain = chains_[session];
        if (!chain.anchor_ms) {
            // Not a live session (recovery): continue from the session's latest
            // known segment, or place a lone file by its write time.
            std::int64_t previous = 0;
            for (auto& s : segments_) if (s.session == session) previous = std::max(previous, s.end_ms);
            chain.frames = 0; chain.fps_num = video.fps_num; chain.fps_den = video.fps_den;
            chain.anchor_ms = previous ? previous : write_time_ms(p) - (chained_ms(0, video.frames, video.fps_num, video.fps_den));
        }
        chain.fps_num = video.fps_num; chain.fps_den = video.fps_den;
        start = chained_ms(chain.anchor_ms, chain.frames, chain.fps_num, chain.fps_den);
        chain.frames += video.frames;
        end = std::max(start + 1, chained_ms(chain.anchor_ms, chain.frames, chain.fps_num, chain.fps_den));
        obs_data_set_int(d.get(), "start_ms", start); obs_data_set_int(d.get(), "end_ms", end);
        obs_data_set_int(d.get(), "frames", video.frames); obs_data_set_double(d.get(), "seconds", (end - start) / 1000.0);
        // Loudness for the editor's audio lane; a failure here is not a reason to lose the segment.
        try { auto levels = audio_levels(p); if (!levels.empty()) { obs_data_set_string(d.get(), "audio_levels", encode_levels(levels).c_str()); obs_data_set_int(d.get(), "audio_bin_ms", AudioBinMs); } } catch (...) {}
        write_json(metadata, d.get());
    }
    segments_.push_back({p, session, start, end, fs::file_size(p)});
    sort();
    return true;
}
void Buffer::configure(const Config& c) {
    if (c.storage != config_.storage) throw std::logic_error("Buffer storage folder changed; rebuild the buffer");
    config_ = c;
}
void Buffer::recover() {
    std::set<fs::path> known; for (auto& s : segments_) known.insert(s.path);
    std::set<fs::path> legacy; // Sidecars from before start_ms was recorded.
    for (auto& session : fs::directory_iterator(root_)) {
        if (!session.is_directory() || reparse(session.path()) || !session.path().filename().wstring().starts_with(L"session-")) continue;
        std::vector<fs::path> files;
        for (auto& entry : fs::directory_iterator(session.path())) {
            if (!entry.is_regular_file() || reparse(entry.path()) || entry.path().extension() != L".mkv" ||
                !entry.path().filename().wstring().starts_with(L"segment-") || known.contains(entry.path())) continue;
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end()); // Segment names carry their wall-clock time, so this is recording order.
        for (auto& file : files) {
            auto sidecar = file; sidecar += L".json";
            bool committed = fs::exists(sidecar);
            if (committed) {
                // A committed sidecar was written after the segment was flushed
                // and probed. Trust it instead of flushing and probing every
                // segment again, which made startup scale with buffer size.
                auto d = read_json(sidecar);
                std::int64_t start = obs_data_get_int(d.get(), "start_ms"), end = obs_data_get_int(d.get(), "end_ms");
                double seconds = obs_data_get_double(d.get(), "seconds");
                if (!(start > 0) && end > 0 && std::isfinite(seconds) && seconds > 0) { start = end - (std::int64_t)std::llround(seconds * 1000); legacy.insert(file); }
                std::error_code size_error; auto bytes = fs::file_size(file, size_error);
                if (start > 0 && end > start && !size_error && bytes > 0) {
                    segments_.push_back({file, path_text(session.path().filename()), start, end, bytes}); continue;
                }
            }
            try {
                if (finalize(file) && !committed) ++recovered;
            } catch (...) {
                // Do not feed an incomplete crash tail into a clip or discard it.
                auto bad = file; bad += L".interrupted";
                std::error_code ec; fs::rename(file, bad, ec); if (!ec) ++quarantined;
            }
        }
    }
    sort();
    // Legacy segments were placed by file write time, which jitters at every
    // seam. Chain them onto their predecessor so runs are contiguous.
    for (size_t i = 1; i < segments_.size(); ++i) {
        auto& s = segments_[i]; auto& previous = segments_[i - 1];
        if (!legacy.contains(s.path) || s.session != previous.session || std::llabs(s.start_ms - previous.end_ms) > 400) continue;
        s.start_ms = std::min(previous.end_ms, s.end_ms - 1);
        auto sidecar = s.path; sidecar += L".json"; auto d = read_json(sidecar);
        obs_data_set_int(d.get(), "start_ms", s.start_ms); obs_data_set_int(d.get(), "end_ms", s.end_ms);
        try { write_json_fast(sidecar, d.get()); } catch (...) {} // One-time migration; a lost rewrite only repeats it.
    }
}
void Buffer::prune(const std::set<fs::path>& pinned) {
    // Leave room inside the quota for the open segment and encoder/keyframe
    // buffering. Metadata is tiny; saved/pending clips have separate lifetimes.
    auto reserve = (std::uintmax_t)config_.max_bitrate * 1000 / 8 * 8;
    auto budget = (std::uintmax_t)(config_.budget_gb * 1e9);
    auto expired = expired_segments(segments_, now_ms(), config_.retention_minutes * 60.0,
        budget > reserve ? budget - reserve : 0, pinned);
    for (auto i = expired.rbegin(); i != expired.rend(); ++i) {
        auto p = segments_[*i].path;
        // Only these files, discovered in our owned buffer, may be removed.
        std::error_code ec; bool removed = fs::remove(p, ec);
        if (!ec && (removed || !fs::exists(p))) {
            auto sidecar = p; sidecar += L".json"; fs::remove(sidecar, ec);
            segments_.erase(segments_.begin() + *i);
        }
    }
}
std::vector<fs::path> Buffer::protect(const std::vector<fs::path>& paths, const fs::path& destination) {
    fs::create_directories(destination); std::vector<fs::path> result;
    for (size_t i = 0; i < paths.size(); ++i) {
        char name[32]; snprintf(name, sizeof(name), "%06zu.mkv", i);
        auto p = destination / name; std::error_code ec;
        fs::create_hard_link(paths[i], p, ec);
        if (ec) fs::copy_file(paths[i], p, fs::copy_options::none);
        result.push_back(p);
    }
    return result;
}
std::uintmax_t Buffer::bytes() const { std::uintmax_t n = 0; for (auto& s : segments_) n += s.bytes; return n; }
double Buffer::seconds() const { double n = 0; for (auto& s : segments_) n += s.seconds(); return n; }
}
