#include "waveform.hpp"
#include <algorithm>

namespace clip {
Waveforms::Waveforms() { worker_ = std::thread([this] { work(); }); }
Waveforms::~Waveforms() {
    { std::lock_guard lock(mutex_); stop_ = true; } wake_.notify_all(); worker_.join();
}
const Waveforms::Wave* Waveforms::levels(const fs::path& segment) {
    std::lock_guard lock(mutex_);
    auto& entry = cache_[segment.wstring()];
    if (entry.ready) return &entry.wave;
    if (!entry.queued) { entry.queued = true; queue_.push_back(segment.wstring()); wake_.notify_one(); }
    return nullptr;
}
void Waveforms::work() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (queue_.empty()) { wake_.wait(lock); continue; }
        auto path = queue_.back(); queue_.pop_back(); // Newest first: it is what is on screen.
        lock.unlock();
        Wave wave;
        try {
            // The sidecar carries the levels for segments closed by the
            // current recorder; older ones are measured from the file.
            auto sidecar = fs::path(path); sidecar += L".json";
            auto d = read_json(sidecar);
            std::string text = obs_data_get_string(d.get(), "audio_levels");
            if (text.empty()) wave.levels = audio_levels(fs::path(path));
            else { wave.levels = decode_levels(text); int bin = (int)obs_data_get_int(d.get(), "audio_bin_ms"); wave.bin_ms = bin > 0 ? bin : 50; }
        } catch (...) { wave = Wave(); }
        lock.lock();
        auto& entry = cache_[path]; entry.wave = std::move(wave); entry.ready = true; entry.queued = false;
    }
}
}
