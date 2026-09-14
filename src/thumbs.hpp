#pragma once
#include "media.hpp"
#include <d3d11.h>
#include <imgui.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <memory>

namespace clip {
// Decoded pictures for the timeline, cached as D3D11 textures. Requests are
// answered from the cache immediately or queued for a worker thread; the
// newest request decodes first because the user is looking there now.
// Keyframe requests are resolved to the keyframe that answers them before
// touching the cache, so every tile spacing, zoom level, and scrub position
// that lands on the same keyframe shares one decode. Decoding runs on a
// private D3D11 device so downloads never block the renderer.
class Thumbnails {
public:
    explicit Thumbnails(ID3D11Device* device);
    ~Thumbnails();
    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;
    struct Picture { ImTextureID texture = 0; int width = 0, height = 0; bool failed = false; };
    // Keyframe at or before offset_ms into the segment (offset -1: exact frame).
    Picture keyframe(const fs::path& segment, std::int64_t offset_ms, int width);
    Picture frame(const fs::path& segment, std::int64_t offset_ms, int width);
    void tick(); // Main thread: upload finished decodes, drop unused entries.
    bool busy() const;
    double last_decode_ms() const { std::lock_guard lock(mutex_); return last_decode_ms_; }
    std::uint64_t decodes() const { std::lock_guard lock(mutex_); return decodes_; }
    size_t cached() const { std::lock_guard lock(mutex_); return cache_.size(); }
private:
    struct Key { std::wstring path; std::int64_t offset; int width; bool exact; auto operator<=>(const Key&) const = default; };
    struct Entry { Picture picture; ID3D11ShaderResourceView* view = nullptr; Frame pending; bool decoded = false, queued = false; std::uint64_t used = 0; };
    // Keyframe positions of a segment, read once from its container index.
    struct Index { std::vector<std::int64_t> keyframes_ms; bool ready = false, queued = false; };
    struct Job { Key key; bool probe = false; };
    Picture request(Key key);
    void work();
    ID3D11Device* device_;
    std::shared_ptr<HwDevice> hw_; // Private D3D11VA device; frames are downloaded for scaling.
    std::unique_ptr<Decoder> decoder_; // Owned by the worker thread.
    std::map<Key, Entry> cache_;
    std::map<std::wstring, Index> index_;
    std::deque<Job> queue_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stop_ = false; int active_ = 0; double last_decode_ms_ = 0; std::uint64_t decodes_ = 0;
    std::uint64_t frame_ = 0;
};
// Keyframe times in milliseconds of a segment's video track, from the
// container index when present, else from a packet scan.
std::vector<std::int64_t> keyframe_times(const fs::path& segment);
}
