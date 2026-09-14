#pragma once
#include "media.hpp"
#include <map>
#include <set>
namespace clip {
// A closed segment with its wall-clock extent. Within a session, segments
// chain exactly: each starts where the previous one ended, and the extent is
// the video track's frame count at its frame rate, so the editor, the player,
// and the export share one time base.
struct Segment {
    fs::path path;
    std::string session;
    std::int64_t start_ms = 0, end_ms = 0;
    std::uintmax_t bytes = 0;
    double seconds() const { return (end_ms - start_ms) / 1000.0; }
};
// Pure policy functions also exercised with small, synthetic quotas in tests.
std::vector<size_t> select_recent(const std::vector<Segment>& segments, double seconds, const std::string& session = {});
std::vector<size_t> expired_segments(const std::vector<Segment>& segments, std::int64_t now, double retention_seconds,
    std::uintmax_t byte_limit, const std::set<fs::path>& pinned);
// Wall-clock start of the segment that follows `frames` frames at `fps` after `anchor_ms`.
std::int64_t chained_ms(std::int64_t anchor_ms, std::int64_t frames, int fps_num, int fps_den);
class Buffer {
public:
    explicit Buffer(const Config& config);
    void recover();
    // The recorder announces a live session's wall-clock origin before its
    // first segment closes; segments of that session chain from it.
    void anchor(const std::string& session, std::int64_t start_ms);
    bool finalize(const fs::path& path);
    // Apply new retention/budget settings. The storage folder must be unchanged.
    void configure(const Config& config);
    void prune(const std::set<fs::path>& pinned = {});
    std::vector<fs::path> protect(const std::vector<fs::path>& paths, const fs::path& destination);
    const std::vector<Segment>& segments() const { return segments_; }
    std::uintmax_t bytes() const;
    double seconds() const;
    fs::path root() const { return root_; }
    int recovered = 0;
    int quarantined = 0;
private:
    struct Chain { std::int64_t anchor_ms = 0, frames = 0; int fps_num = 60, fps_den = 1; };
    Config config_;
    fs::path root_;
    std::vector<Segment> segments_;
    std::map<std::string, Chain> chains_;
    void sort();
};
}
