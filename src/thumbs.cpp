#include "thumbs.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}
#include <algorithm>
#include <chrono>

namespace clip {
std::vector<std::int64_t> keyframe_times(const fs::path& segment) {
    std::vector<std::int64_t> result; AVFormatContext* fmt = nullptr;
    if (!open_without_probe(&fmt, segment)) return result;
    int index = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { index = (int)i; break; }
    if (index >= 0) {
        auto* stream = fmt->streams[index]; auto to_ms = [&](std::int64_t ts) { return av_rescale_q(ts, stream->time_base, AVRational{1, 1000}); };
        int entries = avformat_index_get_entries_count(stream);
        for (int i = 0; i < entries; ++i) { auto* entry = avformat_index_get_entry(stream, i); if (entry->flags & AVINDEX_KEYFRAME) result.push_back(to_ms(entry->timestamp)); }
        if (result.empty()) {
            // No cues (an interrupted file): walk the packets once.
            AVPacket* packet = av_packet_alloc();
            while (av_read_frame(fmt, packet) >= 0) {
                if (packet->stream_index == index && (packet->flags & AV_PKT_FLAG_KEY) && packet->pts != AV_NOPTS_VALUE) result.push_back(to_ms(packet->pts));
                av_packet_unref(packet);
            }
            av_packet_free(&packet);
        }
    }
    avformat_close_input(&fmt);
    std::sort(result.begin(), result.end()); result.erase(std::unique(result.begin(), result.end()), result.end());
    if (result.empty()) result.push_back(0);
    return result;
}
Thumbnails::Thumbnails(ID3D11Device* device) : device_(device) {
    hw_ = hw_device(nullptr);
    worker_ = std::thread([this] { work(); });
}
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
Thumbnails::Picture Thumbnails::request(Key key) {
    std::lock_guard lock(mutex_);
    if (!key.exact) {
        // Resolve to the keyframe that will answer this request; until the
        // segment's index is known, ask for it and show nothing.
        auto& index = index_[key.path];
        if (!index.ready) {
            if (!index.queued) { index.queued = true; queue_.push_back({key, true}); wake_.notify_one(); }
            return {};
        }
        auto it = std::upper_bound(index.keyframes_ms.begin(), index.keyframes_ms.end(), key.offset);
        key.offset = it == index.keyframes_ms.begin() ? index.keyframes_ms.front() : *(it - 1);
    }
    auto& entry = cache_[key]; entry.used = frame_;
    if (!entry.picture.texture && !entry.picture.failed && !entry.decoded && !entry.queued) {
        // A new exact-frame request supersedes queued ones: while a handle is
        // dragged only the latest position matters, and exact decodes are slow.
        if (key.exact) for (auto it = queue_.begin(); it != queue_.end();) {
            if (!it->probe && it->key.exact) { if (auto old = cache_.find(it->key); old != cache_.end()) old->second.queued = false; it = queue_.erase(it); }
            else ++it;
        }
        entry.queued = true; queue_.push_back({key, false}); wake_.notify_one();
    }
    return entry.picture;
}
bool Thumbnails::busy() const { std::lock_guard lock(mutex_); return !queue_.empty() || active_ > 0; }
void Thumbnails::work() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (queue_.empty()) { wake_.wait(lock); continue; }
        // Index probes first: they unblock every tile of a segment.
        auto probe = std::find_if(queue_.begin(), queue_.end(), [](const Job& j) { return j.probe; });
        Job job = probe != queue_.end() ? *probe : queue_.back();
        if (probe != queue_.end()) queue_.erase(probe); else queue_.pop_back();
        ++active_; lock.unlock();
        if (job.probe) {
            std::vector<std::int64_t> times;
            try { times = keyframe_times(fs::path(job.key.path)); } catch (...) { times = {0}; }
            lock.lock(); --active_;
            auto& index = index_[job.key.path]; index.keyframes_ms = std::move(times); index.ready = true; index.queued = false;
            continue;
        }
        Frame frame; bool failed = false; auto began = std::chrono::steady_clock::now();
        try { if (!decoder_) decoder_ = std::make_unique<Decoder>(hw_); frame = decoder_->decode(fs::path(job.key.path), job.key.offset, job.key.width, !job.key.exact); }
        catch (...) { failed = true; }
        lock.lock(); --active_; ++decodes_; last_decode_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        auto it = cache_.find(job.key);
        if (it != cache_.end()) { it->second.pending = std::move(frame); it->second.decoded = true; it->second.picture.failed = failed; it->second.queued = false; }
    }
}
void Thumbnails::tick() {
    std::lock_guard lock(mutex_); ++frame_;
    for (auto& [key, entry] : cache_) {
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
    }
    // Bounded by count only: pictures stay until the cache is full, so
    // panning back over footage seen minutes ago does not decode again.
    constexpr size_t Capacity = 900, Keep = 700;
    if (cache_.size() > Capacity) {
        std::vector<std::uint64_t> ages; ages.reserve(cache_.size());
        for (auto& [key, entry] : cache_) ages.push_back(entry.used);
        std::nth_element(ages.begin(), ages.begin() + (ages.size() - Keep), ages.end());
        auto threshold = ages[ages.size() - Keep];
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->second.used < threshold && !it->second.queued) { if (it->second.view) it->second.view->Release(); it = cache_.erase(it); }
            else ++it;
        }
    }
}
}
