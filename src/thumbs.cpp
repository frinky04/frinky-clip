#include "thumbs.hpp"
#include <algorithm>
#include <chrono>

namespace clip {
Thumbnails::Thumbnails(ID3D11Device* device) : device_(device), worker_([this] { work(); }) {}
Thumbnails::~Thumbnails() {
    { std::lock_guard lock(mutex_); stop_ = true; } wake_.notify_all(); worker_.join();
    for (auto& [key, entry] : cache_) if (entry.view) entry.view->Release();
}
Thumbnails::Picture Thumbnails::keyframe(const fs::path& segment, std::int64_t offset_ms, int width) {
    return request({segment.wstring(), offset_ms, width, false});
}
Thumbnails::Picture Thumbnails::frame(const fs::path& segment, std::int64_t offset_ms, int width) {
    return request({segment.wstring(), offset_ms, width, true});
}
Thumbnails::Picture Thumbnails::request(const Key& key) {
    std::lock_guard lock(mutex_);
    auto& entry = cache_[key]; entry.used = frame_;
    if (!entry.picture.texture && !entry.picture.failed && !entry.decoded && !entry.queued) {
        // A new exact-frame request supersedes queued ones: while a handle is
        // dragged only the latest position matters, and exact decodes are slow.
        if (key.exact) for (auto it = queue_.begin(); it != queue_.end();) {
            if (it->exact) { if (auto old = cache_.find(*it); old != cache_.end()) old->second.queued = false; it = queue_.erase(it); }
            else ++it;
        }
        entry.queued = true; queue_.push_back(key); wake_.notify_one();
    }
    return entry.picture;
}
bool Thumbnails::busy() const { std::lock_guard lock(mutex_); return !queue_.empty() || active_ > 0; }
void Thumbnails::work() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (queue_.empty()) { wake_.wait(lock); continue; }
        Key key = queue_.back(); queue_.pop_back(); ++active_;
        lock.unlock();
        Frame frame; bool failed = false; auto began = std::chrono::steady_clock::now();
        try { frame = decode_frame(fs::path(key.path), key.exact ? key.offset : key.offset, key.width, !key.exact); }
        catch (...) { failed = true; }
        lock.lock(); --active_; last_decode_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        auto it = cache_.find(key);
        if (it != cache_.end()) { it->second.pending = std::move(frame); it->second.decoded = true; it->second.picture.failed = failed; it->second.queued = false; }
    }
}
void Thumbnails::tick() {
    std::lock_guard lock(mutex_); ++frame_;
    for (auto it = cache_.begin(); it != cache_.end();) {
        auto& entry = it->second;
        if (entry.decoded && !entry.picture.failed && !entry.picture.texture) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = entry.pending.width; desc.Height = entry.pending.height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{entry.pending.rgba.data(), (UINT)entry.pending.width * 4, 0};
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(device_->CreateTexture2D(&desc, &init, &texture))) {
                device_->CreateShaderResourceView(texture, nullptr, &entry.view); texture->Release();
                entry.picture.texture = (ImTextureID)entry.view; entry.picture.width = entry.pending.width; entry.picture.height = entry.pending.height;
            } else entry.picture.failed = true;
            entry.pending = Frame();
        }
        // Drop pictures nobody has asked for in a while; keep the cache bounded.
        bool stale = frame_ - entry.used > 600 && !entry.queued;
        if (stale && (cache_.size() > 400 || frame_ - entry.used > 3000)) {
            if (entry.view) entry.view->Release();
            it = cache_.erase(it);
        } else ++it;
    }
}
}
