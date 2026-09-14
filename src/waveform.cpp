#include "waveform.hpp"
#include <algorithm>

namespace clip {
Waveforms::Waveforms() { worker_ = std::thread([this] { work(); }); }
Waveforms::~Waveforms() {
    { std::lock_guard lock(mutex_); stop_ = true; } wake_.notify_all(); worker_.join();
}
const std::vector<std::uint8_t>* Waveforms::levels(const fs::path& segment) {
    std::lock_guard lock(mutex_);
    auto& entry = cache_[segment.wstring()];
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
        std::vector<std::uint8_t> levels;
        try {
            // The sidecar carries the levels for segments closed by the
            // current recorder; older ones are measured from the file.
            auto sidecar = fs::path(path); sidecar += L".json";
            auto d = read_json(sidecar);
            std::string text = obs_data_get_string(d.get(), "audio_levels");
            levels = text.empty() ? audio_levels(fs::path(path)) : decode_levels(text);
        } catch (...) { levels.clear(); }
        lock.lock();
        auto& entry = cache_[path]; entry.levels = std::move(levels); entry.ready = true; entry.queued = false;
    }
}
}
