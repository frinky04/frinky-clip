#include "thumbs.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}
#include <algorithm>
#include <chrono>
#include <climits>
#include <tuple>
#include <cmath>

namespace clip {
constexpr std::int64_t RetryMs = 3000; // A failed decode (a file still being flushed) is tried again after this.
std::vector<std::int64_t> scan_times(const fs::path& segment, const std::function<bool()>& cancelled) {
    std::vector<std::int64_t> result; AVFormatContext* fmt = nullptr;
    // A segment that cannot be opened (expired since the index was read)
    // still gets one entry, so lookups never face an empty list.
    if (!open_without_probe(&fmt, segment)) return {0};
    int index = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { index = (int)i; break; }
    if (index >= 0) {
        auto* stream = fmt->streams[index]; auto to_ms = [&](std::int64_t ts) { return av_rescale_q(ts, stream->time_base, AVRational{1, 1000}); };
        int entries = avformat_index_get_entries_count(stream);
        for (int i = 0; i < entries && result.size() < 65536 && !cancelled(); ++i) { auto* entry = avformat_index_get_entry(stream, i); if (entry->flags & AVINDEX_KEYFRAME) result.push_back(to_ms(entry->timestamp)); }
        if (result.empty()) {
            // No cues (an interrupted file): walk the packets once.
            AVPacket* packet = av_packet_alloc();
            while (!cancelled() && result.size() < 65536 && av_read_frame(fmt, packet) >= 0) {
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
std::vector<std::int64_t> keyframe_times(const fs::path& segment) { return scan_times(segment, [] { return false; }); }
Thumbnails::Thumbnails(ID3D11Device* device) : device_(device) {
    hw_ = hw_device(nullptr);
    worker_ = std::thread([this] { work(); });
}
Thumbnails::~Thumbnails() {
    { std::lock_guard lock(mutex_); stop_ = true; stopping_ = true; ++generation_; } wake_.notify_all(); worker_.join();
}
Thumbnails::Picture Thumbnails::keyframe(const fs::path& segment, std::int64_t offset_ms, int width, int priority) {
    return request({segment.wstring(), offset_ms, width, false}, priority, false);
}
Thumbnails::Picture Thumbnails::frame(const fs::path& segment, std::int64_t offset_ms, int width) {
    return request({segment.wstring(), offset_ms, width, true}, INT_MIN, false);
}
void Thumbnails::prefetch(const fs::path& segment, std::int64_t offset_ms, int width, int priority) {
    request({segment.wstring(), offset_ms, width, false}, priority, true);
}
Thumbnails::Picture Thumbnails::request(Key key, int priority, bool prefetch) {
    std::lock_guard lock(mutex_);
    if (retaining_ && !retained_.contains(key.path)) return {};
    key.width = ((std::clamp(key.width, 1, 1920) + 63) / 64) * 64;
    if (key.exact && (!focus_ || *focus_ != key)) { focus_ = key; ++generation_; }
    prune();

    if (!key.exact) {
        // Resolve to the keyframe that will answer this request; until the
        // segment's index is known, ask for it and show nothing.
        if (!index_.contains(key.path) && index_.size() >= 256) return {};
        auto& index = index_[key.path]; index.used = frame_;
        if (!index.ready) {
            if (!index.queued && queue_.size() < 256) { index.queued = true; index.token = ++next_token_; if (!prefetch) ++foreground_; queue_.push_back({key, true, prefetch ? 2 : 1, priority, frame_, generation_.load(), index.token}); wake_.notify_one(); }
            for (auto& job : queue_) if (job.probe && job.key.path == key.path) { job.rank = std::min(job.rank, prefetch ? 2 : 1); job.priority = priority; job.used = frame_; }
            return {};
        }
        auto it = std::upper_bound(index.keyframes_ms.begin(), index.keyframes_ms.end(), key.offset);
        key.offset = index.keyframes_ms.empty() ? 0 : it == index.keyframes_ms.begin() ? index.keyframes_ms.front() : *(it - 1);
    }
    if (!cache_.contains(key) && cache_.size() >= 768) return {};
    auto& entry = cache_[key]; entry.used = frame_;
    entry.priority = priority;
    for (auto& job : queue_) if (!job.probe && job.key == key) { job.rank = key.exact ? 0 : prefetch ? 2 : 1; job.priority = priority; job.used = frame_; }
    if (entry.picture.failed && steady_ms() - entry.failed_ms > RetryMs) { entry.picture.failed = false; entry.decoded = false; }
    if (!entry.picture.texture && !entry.picture.failed && !entry.decoded && !entry.queued && queue_.size() < 256) {
        // A new exact-frame request supersedes queued ones: while a handle is
        // dragged only the latest position matters, and exact decodes are slow.
        if (key.exact) for (auto it = queue_.begin(); it != queue_.end();) {
            if (!it->probe && it->key.exact) { if (auto old = cache_.find(it->key); old != cache_.end() && old->second.token == it->token) old->second.queued = false; it = queue_.erase(it); }
            else ++it;
        }
        entry.queued = true; entry.token = ++next_token_; if (!prefetch) ++foreground_; queue_.push_back({key, false, key.exact ? 0 : prefetch ? 2 : 1, priority, frame_, generation_.load(), entry.token}); wake_.notify_one();
    }
    if (prefetch) return {};
    if (entry.picture.texture || key.exact) return entry.picture;
    // Not decoded yet: the nearest decoded keyframe of the same segment and
    // width stands in, so a tile never blanks while its own picture is on
    // the way. Keys sort by path, then offset, so neighbours are adjacent.
    Picture best{}; std::int64_t best_distance = INT64_MAX;
    auto consider = [&](const Key& k, const Entry& e) {
        if (k.path != key.path || k.width != key.width || k.exact || !e.picture.texture) return;
        auto distance = std::llabs(k.offset - key.offset);
        if (distance < best_distance) { best_distance = distance; best = e.picture; }
    };
    auto it = cache_.find(key);
    auto forward = std::next(it);
    for (int n = 0; n < 16 && forward != cache_.end() && forward->first.path == key.path; ++n, ++forward) consider(forward->first, forward->second);
    auto backward = it;
    for (int n = 0; n < 16 && backward != cache_.begin(); ++n) { --backward; if (backward->first.path != key.path) break; consider(backward->first, backward->second); }
    return best;
}
bool Thumbnails::busy() const { std::lock_guard lock(mutex_); return !queue_.empty() || active_ > 0; }
namespace {
constexpr size_t GpuBudget = 64ull << 20, ExactBudget = 24ull << 20, PendingBudget = 24ull << 20, ImageBudget = 16ull << 20;
// Bound RGBA allocation before invoking the shared Decoder API. Header dimensions
// also cover portrait footage without imposing a global preview resolution cap.
int admitted_width(const fs::path& path, int requested, double* cached_aspect = nullptr) {
    double aspect = cached_aspect ? *cached_aspect : 0;
    if (aspect <= 0) {
        AVFormatContext* fmt = nullptr;
        if (!open_without_probe(&fmt, path)) throw std::runtime_error("Cannot open thumbnail source");
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            auto* par = fmt->streams[i]->codecpar;
            if (par->codec_type != AVMEDIA_TYPE_VIDEO) continue;
            if (par->width > 0 && par->height > 0) aspect = (double)par->height / par->width;
            break;
        }
        avformat_close_input(&fmt);
        if (aspect <= 0) throw std::runtime_error("Invalid thumbnail dimensions");
        if (cached_aspect) *cached_aspect = aspect;
    }
    const double pixels = ImageBudget / 4;
    int result = std::min(requested, std::max(1, (int)std::sqrt(pixels / aspect)));
    while (result > 0 && (double)result * std::max(1.0, std::round(result * aspect)) > pixels) --result;
    if (result <= 0) throw std::runtime_error("Invalid thumbnail dimensions");
    return result;
}
}
Thumbnails::Stats Thumbnails::stats() const {
    std::lock_guard lock(mutex_); size_t pending = 0;
    for (const auto& [key, entry] : cache_) pending += entry.pending.rgba.size();
    return {gpu_bytes_->load(), pending, queue_.size()};
}
void Thumbnails::prune() {
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->used + 2 < frame_ || (it->key.exact && it->generation != generation_)) {
            if (it->probe) { auto found = index_.find(it->key.path); if (found != index_.end() && found->second.token == it->token) found->second.queued = false; }
            else { auto found = cache_.find(it->key); if (found != cache_.end() && found->second.token == it->token) found->second.queued = false; }
            it = queue_.erase(it);
        } else ++it;
    }
}
void Thumbnails::retain(const std::vector<fs::path>& paths) {
    std::lock_guard lock(mutex_); std::set<std::wstring> next;
    for (const auto& path : paths) next.insert(path.wstring());
    if (retaining_ && next == retained_) return;
    retaining_ = true; retained_ = std::move(next); ++generation_;
    // Invalidate all queued work so a completion from the previous map cannot revive it.
    queue_.clear();
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (!retained_.contains(it->first.path)) it = cache_.erase(it);
        else { it->second.queued = false; it->second.token = ++next_token_; ++it; }
    }
    for (auto it = index_.begin(); it != index_.end();) {
        if (!retained_.contains(it->first)) it = index_.erase(it);
        else { it->second.queued = false; it->second.token = ++next_token_; ++it; }
    }
}
void Thumbnails::work() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (queue_.empty()) { wake_.wait(lock); continue; }
        auto pick = std::min_element(queue_.begin(), queue_.end(), [](const Job& a, const Job& b) {
            return std::tie(a.rank, a.priority) < std::tie(b.rank, b.priority);
        });
        Job job = *pick; queue_.erase(pick); ++active_;
        const auto started_generation = generation_.load();
        const auto foreground = foreground_.load();
        auto cancelled = [this, started_generation, foreground, speculative = job.rank == 2] { return stopping_ || generation_ != started_generation || (speculative && foreground_ != foreground); };
        auto header = index_.find(job.key.path);
        double aspect = header == index_.end() ? 0 : header->second.aspect;
        lock.unlock();
        Frame frame; std::vector<std::int64_t> times; bool failed = false;
        auto began = std::chrono::steady_clock::now();
        try {
            if (job.probe) times = scan_times(fs::path(job.key.path), cancelled);
            else { if (!decoder_) decoder_ = std::make_unique<Decoder>(hw_); const auto path = fs::path(job.key.path);
                const int width = admitted_width(path, job.key.width, &aspect);
                frame = decoder_->decode(path, job.key.offset, width, !job.key.exact, cancelled); }
        } catch (...) { failed = true; }
        lock.lock(); --active_;
        if (cancelled()) {
            if (job.probe) { auto it = index_.find(job.key.path); if (it != index_.end() && it->second.token == job.token) it->second.queued = false; }
            else { auto it = cache_.find(job.key); if (it != cache_.end() && it->second.token == job.token) it->second.queued = false; }
            continue;
        }
        if (job.probe) {
            auto it = index_.find(job.key.path);
            if (it != index_.end() && it->second.token == job.token) { it->second.keyframes_ms = failed ? std::vector<std::int64_t>{0} : std::move(times); it->second.ready = true; it->second.queued = false; }
            continue;
        }
        ++decodes_; last_decode_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        auto it = cache_.find(job.key);
        if (it == cache_.end() || it->second.token != job.token) continue;
        auto& entry = it->second; entry.queued = false;
        if (entry.picture.texture || entry.decoded) continue;
        if (aspect > 0 && (index_.contains(job.key.path) || index_.size() < 256)) {
            auto& index = index_[job.key.path]; index.aspect = aspect; index.used = frame_;
        }
        size_t pending = 0, exact = 0;
        for (const auto& [key, e] : cache_) { pending += e.pending.rgba.size(); if (key.exact) exact += e.pending.rgba.size(); }
        if (frame.rgba.size() > ImageBudget || frame.rgba.size() + pending > PendingBudget || (job.key.exact && frame.rgba.size() + exact > ExactBudget)) continue;
        entry.pending = std::move(frame); entry.decoded = !failed; entry.picture.failed = failed;
        if (failed) entry.failed_ms = steady_ms();
    }
}
void Thumbnails::tick() {
    std::unique_lock lock(mutex_); ++frame_; prune();
    // Eviction occurs before the caller submits this frame's draw commands.
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (!it->second.queued && it->first.exact && focus_ && it->first != *focus_) it = cache_.erase(it);
        else ++it;
    }
    while (index_.size() >= 240) {
        auto oldest = index_.end();
        for (auto it = index_.begin(); it != index_.end(); ++it) if (!it->second.queued && it->second.used + 2 < frame_ && (oldest == index_.end() || it->second.used < oldest->second.used)) oldest = it;
        if (oldest == index_.end()) break;
        index_.erase(oldest);
    }
    while (cache_.size() >= 700) {
        auto oldest = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) if (!it->second.queued && it->second.used + 2 < frame_ && (oldest == cache_.end() || it->second.used < oldest->second.used)) oldest = it;
        if (oldest == cache_.end()) break;
        cache_.erase(oldest);
    }
    for (int uploaded = 0; uploaded < 2; ++uploaded) {
        auto chosen = cache_.end();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (!it->second.decoded || it->second.picture.texture || it->second.pending.rgba.empty()) continue;
            if (chosen == cache_.end() || (it->first.exact && !chosen->first.exact) || (it->first.exact == chosen->first.exact && it->second.priority < chosen->second.priority)) chosen = it;
        }
        if (chosen == cache_.end()) break;
        const Key key = chosen->first; const size_t bytes = chosen->second.pending.rgba.size();
        while (gpu_bytes_->load() + bytes > GpuBudget || (key.exact && exact_bytes_->load() + bytes > ExactBudget)) {
            auto oldest = cache_.end();
            for (auto it = cache_.begin(); it != cache_.end(); ++it)
                if (it != chosen && it->second.picture.ownership && it->second.picture.ownership.use_count() == 1 && (!key.exact || exact_bytes_->load() + bytes <= ExactBudget || it->first.exact) && (oldest == cache_.end() || it->second.used < oldest->second.used)) oldest = it;
            if (oldest == cache_.end()) break;
            cache_.erase(oldest);
        }
        if (gpu_bytes_->load() + bytes > GpuBudget || (key.exact && exact_bytes_->load() + bytes > ExactBudget)) break;
        const auto token = chosen->second.token;
        Frame pending = std::move(chosen->second.pending); const auto generation = generation_.load();
        lock.unlock();
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = pending.width; desc.Height = pending.height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{pending.rgba.data(), (UINT)pending.width * 4, 0};
        ID3D11Texture2D* texture = nullptr; ID3D11ShaderResourceView* view = nullptr;
        if (SUCCEEDED(device_->CreateTexture2D(&desc, &init, &texture))) { device_->CreateShaderResourceView(texture, nullptr, &view); texture->Release(); }
        std::shared_ptr<ID3D11ShaderResourceView> ownership;
        if (view) {
            *gpu_bytes_ += bytes; if (key.exact) *exact_bytes_ += bytes;
            ownership = {view, [total = gpu_bytes_, exact = exact_bytes_, bytes, is_exact = key.exact](ID3D11ShaderResourceView* v) { v->Release(); *total -= bytes; if (is_exact) *exact -= bytes; }};
        }
        lock.lock(); auto it = cache_.find(key);
        if (it == cache_.end() || it->second.token != token || generation != generation_) continue;
        it->second.picture = {(ImTextureID)view, pending.width, pending.height, !view, steady_ms(), std::move(ownership)};
        if (!view) it->second.failed_ms = steady_ms();
    }
}
std::string thumbnail_test(ID3D11Device* device, const fs::path& segment) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
    Thumbnails thumbs(device);
    thumbs.retain({segment});
    auto bounded = [&] {
        const auto stats = thumbs.stats();
        return stats.cached_bytes <= GpuBudget && stats.pending_bytes <= PendingBudget && stats.queued <= 256;
    };
    auto await_picture = [&](std::int64_t offset, int width, bool exact) {
        Thumbnails::Picture picture;
        while (std::chrono::steady_clock::now() < deadline) {
            thumbs.tick();
            picture = exact ? thumbs.frame(segment, offset, width) : thumbs.keyframe(segment, offset, width);
            if (picture.texture || picture.failed || !bounded()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        return picture;
    };
    // Different requested widths in one bucket must share the actual uploaded SRV.
    auto tile = await_picture(0, 257, false);
    if (!tile.texture) return "thumbnail: keyframe did not upload before deadline";
    auto same = thumbs.keyframe(segment, 0, 319);
    if (same.texture != tile.texture || tile.width != 320) return "thumbnail: width bucket failed to share texture";
    const size_t tile_bytes = (size_t)tile.width * tile.height * 4;
    tile = {}; same = {};

    // Let an exact request enter the worker, then replace it repeatedly. Only
    // the final requested key should become the retained exact cache entry.
    thumbs.frame(segment, 0, 1920);
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    for (int i = 1; i < 32; ++i) thumbs.frame(segment, i * 7, 1920);
    auto final = await_picture(217, 1920, true);
    if (!final.texture) return "thumbnail: superseded exact requests did not reach final target";
    if (final.width != admitted_width(segment, 1920)) return "thumbnail: large preview resolution was reduced";
    final = {};
    auto retained = await_picture(217, 640, true);
    if (!retained.texture) return "thumbnail: small retained preview did not upload";
    for (int i = 0; i < 6; ++i) {
        auto picture = await_picture(300 + i * 33, 1920, true);
        if (!picture.texture) return "thumbnail: repeated large exact request did not upload";
        if (!bounded() || thumbs.stats().cached_bytes > ExactBudget + tile_bytes)
            return "thumbnail: large exact requests exceeded byte budget";
    }
    // A retained path may receive a replacement while its old job is being
    // cancelled. Completion must only mutate the matching job token.
    const fs::path unused_path = segment.wstring() + L".thumbnail-test-unused";
    for (int i = 0; i < 8; ++i) {
        const auto offset = 600 + i * 19;
        thumbs.frame(segment, offset, 1920);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        thumbs.retain({segment, unused_path});
        thumbs.frame(segment, offset, 1920);
        thumbs.retain({segment});
        thumbs.frame(segment, offset, 1920);
    }
    auto replaced = await_picture(733, 1920, true);
    if (!replaced.texture) return "thumbnail: kept-path replacement did not upload";
    replaced = {};
    while (thumbs.busy() && std::chrono::steady_clock::now() < deadline) {
        thumbs.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    thumbs.tick();
    if (thumbs.busy() || thumbs.stats().pending_bytes != 0)
        return "thumbnail: kept-path replacement left duplicate pending pixels";
    // Removing the map while work is active must neither resurrect entries nor
    // invalidate a copied Picture still held by the UI.
    thumbs.frame(segment, 550, 1920);
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    thumbs.retain({});
    while (thumbs.busy() && std::chrono::steady_clock::now() < deadline) {
        thumbs.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    thumbs.tick();
    if (thumbs.busy()) return "thumbnail: removed active request failed to cancel before deadline";
    if (thumbs.cached() != 0 || thumbs.stats().pending_bytes != 0 || thumbs.stats().queued != 0)
        return "thumbnail: removed work resurrected cache data";
    ID3D11Resource* resource = nullptr;
    if (retained.ownership) retained.ownership->GetResource(&resource);
    if (!resource) return "thumbnail: retained Picture lost its GPU resource";
    resource->Release();
    if (thumbs.stats().cached_bytes == 0) return "thumbnail: retained GPU resource missing from accounting";
    retained = {};
    if (thumbs.stats().cached_bytes != 0) return "thumbnail: GPU bytes remained after final Picture release";
    return {};
}

}
