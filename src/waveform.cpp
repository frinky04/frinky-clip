#include "waveform.hpp"
#include <algorithm>
#include <set>

namespace clip {
Waveforms::Waveforms() { worker_ = std::thread([this] { work(); }); }
Waveforms::~Waveforms() {
    { std::lock_guard lock(mutex_); stop_ = true; } wake_.notify_all(); worker_.join();
}
const std::vector<AudioLevels>* Waveforms::levels(const fs::path& segment) {
    std::lock_guard lock(mutex_);
    auto& entry = cache_[segment.wstring()];
    // A segment the recorder had not measured yet is read again after a while.
    if (entry.ready && entry.levels.empty() && steady_ms() - entry.read_ms > 5000) entry.ready = false;
    if (entry.ready) return entry.levels.empty() ? nullptr : &entry.levels;
    if (!entry.queued) { entry.queued = true; entry.generation = ++generation_; queue_.emplace_back(segment.wstring(), entry.generation); wake_.notify_one(); }
    return nullptr;
}
void Waveforms::retain(const std::vector<fs::path>& paths) {
    std::set<std::wstring> keep; for (const auto& path : paths) keep.insert(path.wstring());
    std::lock_guard lock(mutex_);
    std::erase_if(cache_, [&](const auto& item) { return !keep.contains(item.first); });
    std::erase_if(queue_, [&](const auto& item) { return !keep.contains(item.first); });
    ++revision_;
}
void Waveforms::work() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (queue_.empty()) { wake_.wait(lock); continue; }
        auto [path, generation] = queue_.back(); queue_.pop_back(); // Newest first: it is what is on screen.
        lock.unlock();
        std::vector<AudioLevels> levels;
        try { auto sidecar = fs::path(path); sidecar += L".json"; levels = read_levels(read_json(sidecar).get()); } catch (...) { levels.clear(); }
        lock.lock();
        auto found = cache_.find(path);
        if (found == cache_.end() || found->second.generation != generation) continue;
        auto& entry = found->second; entry.levels = std::move(levels); entry.ready = true; entry.queued = false; entry.read_ms = steady_ms(); ++revision_;
    }
}
}
