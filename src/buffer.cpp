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
        result.push_back(i-1); remaining -= segments[i-1].seconds;
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
Buffer::Buffer(const Config& c) : config_(c), root_(c.storage / "buffer") {
    if (fs::exists(root_) && !fs::exists(root_ / ".frinky-buffer") && !fs::is_empty(root_))
        throw std::runtime_error("Buffer directory contains unrelated files. Choose another storage folder.");
    fs::create_directories(root_);
    if (!fs::exists(root_ / ".frinky-buffer")) atomic_write(root_ / ".frinky-buffer", "Frinky Clip buffer v1\n");
    fs::create_directories(config_.storage / "clips"); fs::create_directories(config_.storage / "pending");
}
static bool reparse(const fs::path& path) { return (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0; }
bool Buffer::finalize(const fs::path& p) {
    if (!fs::exists(p) || !flush_closed(p)) return false;
    if (std::any_of(segments_.begin(), segments_.end(), [&](auto& s) { return s.path == p; })) return true;
    MediaInfo media = inspect_media(p);
    auto metadata = p; metadata += L".json";
    auto d = read_json(metadata);
    std::int64_t end = obs_data_get_int(d.get(), "end_ms");
    if (!end) {
        auto stamp = fs::last_write_time(p);
        end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::clock_cast<std::chrono::system_clock>(stamp).time_since_epoch()).count();
    }
    obs_data_set_int(d.get(), "end_ms", end); obs_data_set_double(d.get(), "seconds", media.seconds);
    write_json(metadata, d.get());
    segments_.push_back({p, path_text(p.parent_path().filename()), end, media.seconds, fs::file_size(p)});
    std::sort(segments_.begin(), segments_.end(), [](auto& a, auto& b) { return a.end_ms != b.end_ms ? a.end_ms < b.end_ms : a.path < b.path; });
    return true;
}
void Buffer::configure(const Config& c) {
    if (c.storage != config_.storage) throw std::logic_error("Buffer storage folder changed; rebuild the buffer");
    config_ = c;
}
void Buffer::recover() {
    std::set<fs::path> known; for (auto& s : segments_) known.insert(s.path);
    for (auto& session : fs::directory_iterator(root_)) {
        if (!session.is_directory() || reparse(session.path()) || !session.path().filename().wstring().starts_with(L"session-")) continue;
        for (auto& entry : fs::directory_iterator(session.path())) {
            if (!entry.is_regular_file() || reparse(entry.path()) || entry.path().extension() != L".mkv" ||
                !entry.path().filename().wstring().starts_with(L"segment-") || known.contains(entry.path())) continue;
            auto sidecar = entry.path(); sidecar += L".json";
            bool committed = fs::exists(sidecar);
            if (committed) {
                // A committed sidecar was written after the segment was flushed
                // and probed. Trust it instead of flushing and probing every
                // segment again, which made startup scale with buffer size.
                auto d = read_json(sidecar);
                std::int64_t end = obs_data_get_int(d.get(), "end_ms"); double seconds = obs_data_get_double(d.get(), "seconds");
                std::error_code size_error; auto bytes = fs::file_size(entry.path(), size_error);
                if (end > 0 && std::isfinite(seconds) && seconds > 0 && !size_error && bytes > 0) {
                    segments_.push_back({entry.path(), path_text(session.path().filename()), end, seconds, bytes}); continue;
                }
            }
            try {
                if (finalize(entry.path()) && !committed) ++recovered;
            } catch (...) {
                // Do not feed an incomplete crash tail into a clip or discard it.
                auto bad = entry.path(); bad += L".interrupted";
                std::error_code ec; fs::rename(entry.path(), bad, ec); if (!ec) ++quarantined;
            }
        }
    }
    std::sort(segments_.begin(), segments_.end(), [](auto& a, auto& b) { return a.end_ms != b.end_ms ? a.end_ms < b.end_ms : a.path < b.path; });
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
double Buffer::seconds() const { double n = 0; for (auto& s : segments_) n += s.seconds; return n; }
}
