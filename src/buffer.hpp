#pragma once
#include "media.hpp"
#include <set>
namespace clip {
struct Segment {
    fs::path path;
    std::string session;
    std::int64_t end_ms = 0;
    double seconds = 0;
    std::uintmax_t bytes = 0;
};
// Pure policy functions also exercised with small, synthetic quotas in tests.
std::vector<size_t> select_recent(const std::vector<Segment>& segments, double seconds, const std::string& session = {});
std::vector<size_t> expired_segments(const std::vector<Segment>& segments, std::int64_t now, double retention_seconds,
    std::uintmax_t byte_limit, const std::set<fs::path>& pinned);
class Buffer {
public:
    explicit Buffer(const Config& config);
    void recover();
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
    Config config_;
    fs::path root_;
    std::vector<Segment> segments_;
};
}

