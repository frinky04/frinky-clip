#include "waveform.hpp"
#include <algorithm>

namespace clip {
Waveforms::Waveforms() { worker_ = std::thread([this] { work(); }); }
Waveforms::~Waveforms() {
    { std::lock_guard lock(mutex_); stop_ = true; } wake_.notify_all(); worker_.join();
}
const AudioLevels* Waveforms::levels(const fs::path& segment) {
    std::lock_guard lock(mutex_);
    auto& entry = cache_[segment.wstring()];
    // A segment the recorder had not measured yet is read again after a while.
    if (entry.ready && entry.levels.empty() && steady_ms() - entry.read_ms > 5000) entry.ready = false;
    if (entry.ready) return &entry.levels;
    if (!entry.queued) { entry.queued = true; queue_.push_back(segment.wstring()); wake_.notify_one(); }
    return nullptr;
}
void Waveforms::work() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (queue_.empty()) { wake_.wait(lock); continue; }
        auto path = queue_.back(); queue_.pop_back(); // Newest first: it is what is on screen.
        lock.unlock();
        AudioLevels levels;
        try { auto sidecar = fs::path(path); sidecar += L".json"; levels = read_levels(read_json(sidecar).get()); } catch (...) { levels = AudioLevels(); }
        lock.lock();
        auto& entry = cache_[path]; entry.levels = std::move(levels); entry.ready = true; entry.queued = false; entry.read_ms = steady_ms();
    }
}
}
