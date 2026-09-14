#pragma once
#include "media.hpp"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace clip {
// Loudness per segment for the audio lane. The recorder writes levels into
// each segment's sidecar as it closes; segments from before that are
// measured here on demand. Loading runs on a worker thread; the lane draws
// what is ready and asks again next frame for the rest.
class Waveforms {
public:
    Waveforms();
    ~Waveforms();
    Waveforms(const Waveforms&) = delete;
    Waveforms& operator=(const Waveforms&) = delete;
    // Levels for a segment (one byte per AudioBinMs), or nullptr until loaded.
    // The pointer stays valid for the lifetime of this object.
    const std::vector<std::uint8_t>* levels(const fs::path& segment);
private:
    struct Entry { std::vector<std::uint8_t> levels; bool ready = false, queued = false; };
    void work();
    std::map<std::wstring, Entry> cache_;
    std::deque<std::wstring> queue_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stop_ = false;
};
}
