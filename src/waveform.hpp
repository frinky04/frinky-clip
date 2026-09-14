#pragma once
#include "media.hpp"
#include <condition_variable>
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
    // valid for the lifetime of this object; an unmeasured segment is empty.
    const std::vector<AudioLevels>* levels(const fs::path& segment);
private:
    struct Entry { std::vector<AudioLevels> levels; bool ready = false, queued = false; std::int64_t read_ms = 0; };
    void work();
    std::map<std::wstring, Entry> cache_;
    std::deque<std::wstring> queue_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stop_ = false;
};
}
