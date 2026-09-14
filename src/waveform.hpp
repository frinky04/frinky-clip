#pragma once
#include "media.hpp"
#include <condition_variable>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace clip {
// The fine peak file per segment for the audio lane, read from the sidecar
// on a worker thread when the view is zoomed in enough to need it. The
// coarse level travels with the segment index and needs no loading; the
// recorder measures segments that have no peak file yet.
class Waveforms {
public:
    Waveforms();
    ~Waveforms();
    Waveforms(const Waveforms&) = delete;
    Waveforms& operator=(const Waveforms&) = delete;
    // Fine levels for a segment, or nullptr until read. The pointer stays
    // valid until the next retain() call on the main thread. Published nonempty
    // vectors are immutable; an unmeasured segment returns nullptr.
    const std::vector<AudioLevels>* levels(const fs::path& segment);
    void retain(const std::vector<fs::path>& paths);
    std::uint64_t revision() const { return revision_.load(); }
private:
    struct Entry { std::vector<AudioLevels> levels; bool ready = false, queued = false; std::int64_t read_ms = 0; std::uint64_t generation = 0; };
    void work();
    std::map<std::wstring, Entry> cache_;
    std::deque<std::pair<std::wstring, std::uint64_t>> queue_;
    std::uint64_t generation_ = 0;
    std::atomic<std::uint64_t> revision_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stop_ = false;
};
}
