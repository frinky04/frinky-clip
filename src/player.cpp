#include "player.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
}
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace clip {
namespace {
constexpr size_t QueueFrames = 12;       // ~200 ms of decoded video ahead of the playhead.
constexpr REFERENCE_TIME AudioBuffer = 5000000; // 500 ms shared-mode render buffer.
// Full-screen triangle and BT.709 limited-range NV12 to RGB.
const char* ConvertShader = R"(
Texture2DArray<float> luma : register(t0);
Texture2DArray<float2> chroma : register(t1);
SamplerState samp : register(s0);
struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o;
}
float4 ps(V i) : SV_TARGET {
    float y = (luma.Sample(samp, float3(i.uv, 0)) - 16.0 / 255.0) * 1.164;
    float2 c = chroma.Sample(samp, float3(i.uv, 0)) - 0.5;
    return float4(saturate(y + 1.793 * c.y), saturate(y - 0.213 * c.x - 0.533 * c.y), saturate(y + 2.112 * c.x), 1);
})";
std::shared_ptr<AVFrame> hold(AVFrame* frame) {
    AVFrame* copy = av_frame_alloc(); av_frame_ref(copy, frame);
    return std::shared_ptr<AVFrame>(copy, [](AVFrame* f) { av_frame_free(&f); });
}
}

// Shared-mode WASAPI output in the device's mix format; decoded audio is
// converted to it with swresample.
struct Player::Audio {
    IAudioClient* client = nullptr; IAudioRenderClient* render = nullptr; WAVEFORMATEX* format = nullptr;
    UINT32 buffer_frames = 0; bool started = false, running = false, is_float = false, failed = false; std::int64_t written = 0, media_start = -1;
    bool init() {
        IMMDeviceEnumerator* devices = nullptr; IMMDevice* device = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&devices))) return false;
        bool ok = SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) &&
            SUCCEEDED(client->GetMixFormat(&format)) && SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, AudioBuffer, 0, format, nullptr)) &&
            SUCCEEDED(client->GetBufferSize(&buffer_frames)) && SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), (void**)&render));
        if (ok) {
            is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        }
        if (device) device->Release(); if (devices) devices->Release();
        return ok;
    }
    bool accept(HRESULT result) { if (FAILED(result)) failed = true; return !failed; }
    void reset() { if (client && !failed) { accept(client->Stop()); if (!failed) accept(client->Reset()); } started = running = false; written = 0; media_start = -1; }
    void suspend() { if (client && running && !failed) accept(client->Stop()); running = false; }
    void resume() { if (client && started && !running && !failed) { running = accept(client->Start()); } }
    void start_tail() { if (client && !started && written > 0 && !failed) { started = running = accept(client->Start()); } }
    // Frames of the pending block that fit now; 0 when the device buffer is full.
    UINT32 write(const std::uint8_t* data, UINT32 frames) {
        UINT32 padding = 0; if (failed || !accept(client->GetCurrentPadding(&padding))) return 0;
        if (padding > buffer_frames) { failed = true; return 0; }
        UINT32 n = std::min(frames, buffer_frames - padding); if (!n) return 0;
        BYTE* out = nullptr; if (!accept(render->GetBuffer(n, &out))) return 0;
        memcpy(out, data, (size_t)n * format->nBlockAlign); if (!accept(render->ReleaseBuffer(n, 0))) return 0; written += n;
        if (!started && written >= (std::int64_t)buffer_frames / 4) { started = running = accept(client->Start()); }
        return n;
    }
    std::int64_t played_ms() {
        if (failed || !started || media_start < 0) return -1;
        UINT32 padding = 0; if (!accept(client->GetCurrentPadding(&padding))) return -1;
        return media_start + (written - (std::int64_t)padding) * 1000 / format->nSamplesPerSec;
    }
    void close() { if (client) client->Stop(); if (render) render->Release(); if (client) client->Release(); if (format) CoTaskMemFree(format); render = nullptr; client = nullptr; format = nullptr; }
    ~Audio() { close(); }
};

struct Player::Source {
    AVFormatContext* fmt = nullptr; AVCodecContext* video = nullptr;
    AVPacket* packet = av_packet_alloc(); AVFrame* frame = av_frame_alloc();
    SwsContext* sws = nullptr; int sws_w = 0, sws_h = 0, sws_src_w = 0, sws_src_h = 0, sws_fmt = -1;
    int vindex = -1; Span span; bool eof = false, drained = false, failed = false;
    std::int64_t last_video_ms = -1;
    HwBinding binding;
    // Audio tracks (desktop, then microphone), each decoded and converted to
    // the device's rate and channels as interleaved floats. `start` is the
    // grid index (device-rate samples from the segment start) of the queue's
    // first sample; the mix walks the grid in step across tracks.
    struct Track { int index = -1; AVCodecContext* ctx = nullptr; SwrContext* swr = nullptr; std::vector<float> queue; std::int64_t start = -1; bool done = false; };
    std::vector<Track> tracks; std::int64_t mixed = -1; std::vector<float> mixbuf;
    std::vector<std::uint8_t> pending; // Mixed audio in the device format, not yet accepted by the device.
    size_t pending_offset = 0;
    bool video_hw = false; // Whether the kept video decoder is the hardware one.
    void reset_audio() { for (auto& t : tracks) { t.queue.clear(); t.start = -1; t.done = false; } mixed = -1; pending.clear(); pending_offset = 0; }
    void close(bool keep_video = false) {
        sws_freeContext(sws); sws = nullptr;
        if (!keep_video) avcodec_free_context(&video);
        for (auto& t : tracks) { swr_free(&t.swr); avcodec_free_context(&t.ctx); }
        tracks.clear(); avformat_close_input(&fmt);
        vindex = -1; eof = drained = failed = false; last_video_ms = -1; reset_audio();
    }
    ~Source() { close(); av_packet_free(&packet); av_frame_free(&frame); }
    std::int64_t ms(AVFrame* f, int index) const {
        std::int64_t pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
        return pts == AV_NOPTS_VALUE ? -1 : span.start_ms + av_rescale_q(pts, fmt->streams[index]->time_base, AVRational{1, 1000});
    }
};

Player::Player(ID3D11Device* device, ID3D11DeviceContext* context) : device_(device), context_(context) {
    // The decoder submits work on the immediate context from its own thread;
    // the device's internal lock serializes that with rendering.
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(context_->QueryInterface(__uuidof(ID3D11Multithread), (void**)&multithread))) { multithread->SetMultithreadProtected(TRUE); multithread->Release(); }
    hw_ = hw_device(device_); hardware_ = hw_ != nullptr;
    worker_ = std::thread([this] { work(); });
}
Player::~Player() {
    stop_ = true; wake_.notify_all(); space_.notify_all(); worker_.join();
    ready_.clear(); shown_.reset();
    for (IUnknown* object : {(IUnknown*)view_, (IUnknown*)target_, (IUnknown*)texture_, (IUnknown*)vs_, (IUnknown*)ps_, (IUnknown*)sampler_}) if (object) object->Release();
}
void Player::set_spans(std::vector<Span> spans) { std::lock_guard lock(mutex_); spans_ = std::make_shared<const std::vector<Span>>(std::move(spans)); }
std::int64_t Player::playback_end() const {
    auto end = end_ms_.load(), out = range_end_.load();
    return out && end ? std::min(end, out) : out ? out : end;
}
void Player::set_range(std::int64_t in, std::int64_t out) {
    if (!in || out <= in) in = out = 0;
    std::lock_guard lock(mutex_);
    if (range_start_ == in && range_end_ == out) return;
    // Editing changes the next playback, never the user's inspection position.
    if (playing_) { position_ = position(); playing_ = false; }
    range_start_ = in; range_end_ = out; restart_decoder_ = true;
    space_.notify_all(); wake_.notify_all();
}
void Player::seek(std::int64_t ms) {
    std::lock_guard lock(mutex_);
    request_seek(ms);
}
void Player::request_seek(std::int64_t ms) {
    // The playhead moves now; the picture catches up. Like netcode: the
    // UI state is authoritative and the decoder converges on it. Playback
    // continues from the new position when it was playing.
    seek_target_ = ms; position_ = ms; pending_seek_ = true; seeking_ = true; resumable_ = false;
    drain_end_ms_ = 0; restart_decoder_ = false; ++generation_;
    wake_.notify_all(); space_.notify_all();
}
void Player::play() {
    std::lock_guard lock(mutex_);
    if (playing_) return;
    auto at = position_.load(), in = range_start_.load(), out = range_end_.load();
    if (out && (at < in || at >= out)) request_seek(in);
    else if (restart_decoder_) request_seek(at);
    clock_media_ = position_.load(); clock_wall_ = steady_ms();
    // A clean pause left the decoder and queue in place: continue from there.
    if (!resumable_) { seek_target_ = position_.load(); pending_seek_ = true; }
    playing_ = true; ++generation_;
    wake_.notify_all(); space_.notify_all();
}
void Player::pause() {
    std::lock_guard lock(mutex_);
    if (!playing_) return;
    position_ = position(); playing_ = false; space_.notify_all(); wake_.notify_all();
}
std::int64_t Player::position() const {
    if (pending_seek_) return seek_target_;
    if (!playing_) return position_;
    auto wall = clock_media_ + (steady_ms() - clock_wall_);
    std::lock_guard lock(audio_clock_mutex_);
    if (audio_time_ >= 0) return drain_end_ms_ ? std::max(audio_time_, wall) : audio_time_;
    return wall;
}
bool Player::reposition(Source& src, std::int64_t offset_ms) {
    auto target = av_rescale_q(std::max<std::int64_t>(0, offset_ms), AVRational{1, 1000}, src.fmt->streams[src.vindex]->time_base);
    if (av_seek_frame(src.fmt, src.vindex, target, AVSEEK_FLAG_BACKWARD) < 0) return false;
    avcodec_flush_buffers(src.video); for (auto& t : src.tracks) if (t.ctx) avcodec_flush_buffers(t.ctx);
    src.eof = src.drained = src.failed = false; src.last_video_ms = -1; src.reset_audio();
    return true;
}
bool Player::open(Source& src, const Span& span, std::int64_t offset_ms) {
    AVFormatContext* fmt = nullptr; int vindex = -1; std::vector<int> audio_streams;
    if (!open_without_probe(&fmt, span.path)) { src.close(); return false; }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        auto* par = fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && vindex < 0) vindex = (int)i;
        if (par->codec_type == AVMEDIA_TYPE_AUDIO) audio_streams.push_back((int)i);
    }
    if (vindex < 0) { avformat_close_input(&fmt); src.close(); return false; }
    bool hardware = hw_ && hw_ok_;
    // Segments of a session share stream parameters: keep the video decoder
    // and its surface pool, since rebuilding them costs tens of milliseconds
    // at every seek and segment hand-off.
    auto* vpar = fmt->streams[vindex]->codecpar;
    bool reuse = src.video && src.video_hw == hardware && src.video->codec_id == vpar->codec_id && src.video->width == vpar->width && src.video->height == vpar->height &&
        src.video->extradata_size == vpar->extradata_size && (vpar->extradata_size == 0 || memcmp(src.video->extradata, vpar->extradata, vpar->extradata_size) == 0);
    src.close(reuse); src.span.path = span.path; src.span.session = span.session; src.span.start_ms = span.start_ms; src.span.end_ms = span.end_ms; src.fmt = fmt; src.vindex = vindex;
    auto make = [&](int index, AVCodecContext*& ctx, bool video) {
        auto* par = src.fmt->streams[index]->codecpar; const AVCodec* codec = pick_decoder(par->codec_id, video && hardware);
        if (!codec || !(ctx = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(ctx, par) < 0) return false;
        ctx->thread_count = (video && hardware) ? 1 : 0;
        if (video && hardware) { src.binding = {hw_, true, (int)QueueFrames + 16}; ctx->opaque = &src.binding; ctx->get_format = hw_get_format; }
        return avcodec_open2(ctx, codec, nullptr) >= 0;
    };
    if (reuse) avcodec_flush_buffers(src.video);
    else if (!make(src.vindex, src.video, true)) return false;
    src.video_hw = hardware;
    // Up to two audio tracks, desktop then microphone, each converted to
    // interleaved float at the device's rate and channels for the mix.
    if (audio_) for (size_t k = 0; k < std::min<size_t>(2, audio_streams.size()); ++k) {
        Source::Track track; track.index = audio_streams[k];
        if (!make(track.index, track.ctx, false)) { avcodec_free_context(&track.ctx); continue; }
        AVChannelLayout out{}; av_channel_layout_default(&out, audio_->format->nChannels);
        bool ok = swr_alloc_set_opts2(&track.swr, &out, AV_SAMPLE_FMT_FLT, (int)audio_->format->nSamplesPerSec,
            &track.ctx->ch_layout, (AVSampleFormat)track.ctx->sample_fmt, track.ctx->sample_rate, 0, nullptr) >= 0 && swr_init(track.swr) >= 0;
        av_channel_layout_uninit(&out);
        if (!ok) { swr_free(&track.swr); avcodec_free_context(&track.ctx); continue; }
        src.tracks.push_back(track);
    }
    if (offset_ms > 0) {
        auto target = av_rescale_q(offset_ms, AVRational{1, 1000}, src.fmt->streams[src.vindex]->time_base);
        av_seek_frame(src.fmt, src.vindex, target, AVSEEK_FLAG_BACKWARD);
    }
    return true;
}
void Player::push_audio(Source& src, size_t k, void* f) {
    auto* frame = static_cast<AVFrame*>(f); auto& track = src.tracks[k];
    int channels = audio_->format->nChannels, rate = (int)audio_->format->nSamplesPerSec;
    if (track.start < 0) {
        std::int64_t pts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) return;
        track.start = av_rescale_q(pts, src.fmt->streams[track.index]->time_base, AVRational{1, rate});
    }
    int out_samples = swr_get_out_samples(track.swr, frame->nb_samples); if (out_samples <= 0) return;
    size_t old = track.queue.size(); track.queue.resize(old + (size_t)out_samples * channels);
    std::uint8_t* out = reinterpret_cast<std::uint8_t*>(track.queue.data() + old);
    int got = swr_convert(track.swr, &out, out_samples, (const std::uint8_t**)frame->data, frame->nb_samples);
    track.queue.resize(old + (size_t)std::max(0, got) * channels);
    mix_audio(src, false);
}
// Sum the tracks' queues in step onto the device queue: without `flush`, as
// far as every track has samples; with it, everything that is left, with
// silence for a track that has none. Gains apply per track.
void Player::mix_audio(Source& src, bool flush) {
    int channels = audio_->format->nChannels, rate = (int)audio_->format->nSamplesPerSec;
    if (src.mixed < 0) {
        std::int64_t first = -1;
        for (auto& t : src.tracks) if (t.start >= 0 && (first < 0 || t.start < first)) first = t.start;
        if (first < 0) return;
        src.mixed = first; if (audio_->media_start < 0) audio_->media_start = src.span.start_ms + av_rescale(first, 1000, rate);
    }
    std::int64_t n = flush ? 0 : INT64_MAX;
    for (auto& t : src.tracks) {
        if (!t.ctx) continue;
        std::int64_t have = t.start < 0 ? 0 : t.start + (std::int64_t)(t.queue.size() / channels) - src.mixed;
        n = flush ? std::max(n, have) : std::min(n, have);
    }
    if (n <= 0 || n == INT64_MAX) return;
    src.mixbuf.assign((size_t)(n * channels), 0.f);
    for (size_t k = 0; k < src.tracks.size(); ++k) {
        auto& t = src.tracks[k]; if (t.start < 0) continue;
        float gain = gains_[std::min<size_t>(k, 1)].load(); std::int64_t frames_in_queue = (std::int64_t)(t.queue.size() / channels);
        for (std::int64_t i = 0; i < n; ++i) {
            std::int64_t at = src.mixed + i - t.start; if (at < 0 || at >= frames_in_queue) continue;
            for (int c = 0; c < channels; ++c) src.mixbuf[(size_t)(i * channels + c)] += t.queue[(size_t)(at * channels + c)] * gain;
        }
        std::int64_t drop = std::clamp<std::int64_t>(src.mixed + n - t.start, 0, frames_in_queue);
        t.queue.erase(t.queue.begin(), t.queue.begin() + (std::ptrdiff_t)(drop * channels)); t.start += drop;
    }
    src.mixed += n;
    size_t block = audio_->format->nBlockAlign, old = src.pending.size(); src.pending.resize(old + (size_t)n * block);
    if (audio_->is_float) { auto* out = reinterpret_cast<float*>(src.pending.data() + old); for (size_t i = 0; i < src.mixbuf.size(); ++i) out[i] = std::clamp(src.mixbuf[i], -1.f, 1.f); }
    else { auto* out = reinterpret_cast<std::int16_t*>(src.pending.data() + old); for (size_t i = 0; i < src.mixbuf.size(); ++i) out[i] = (std::int16_t)std::lround(std::clamp(src.mixbuf[i], -1.f, 1.f) * 32767.f); }
}
// Decode until one video frame is produced (returned in out_video) or the
// file ends. Audio frames are converted and queued along the way.
bool Player::step(Source& src, Decoded* out_video, bool want_audio) {
    while (true) {
        for (size_t slot = 0; slot <= src.tracks.size(); ++slot) {
            AVCodecContext* ctx = slot == 0 ? src.video : src.tracks[slot - 1].ctx;
            if (!ctx) continue;
            int r = avcodec_receive_frame(ctx, src.frame);
            if (r < 0) {
                if (r == AVERROR_EOF && ctx == src.video) src.drained = true;
                else if (r == AVERROR_EOF) {
                    // A track ran dry at the segment's end: once all have, flush the mix.
                    src.tracks[slot - 1].done = true;
                    if (want_audio && audio_ && std::all_of(src.tracks.begin(), src.tracks.end(), [](auto& t) { return !t.ctx || t.done; })) mix_audio(src, true);
                }
                else if (r != AVERROR(EAGAIN) && ctx == src.video) { src.failed = true; return false; }
                continue;
            }
            if (ctx == src.video) {
                std::int64_t ms = src.ms(src.frame, src.vindex); src.last_video_ms = ms; out_video->ms = ms;
                hardware_ = src.frame->format == AV_PIX_FMT_D3D11;
                if (src.frame->format == AV_PIX_FMT_D3D11) {
                    out_video->hw = hold(src.frame); out_video->width = src.frame->width; out_video->height = src.frame->height;
                } else {
                    int width = std::max(64, width_.load()), height = std::max(1, (int)std::lround((double)src.frame->height * width / std::max(1, src.frame->width)));
                    if (!src.sws || src.sws_w != width || src.sws_src_w != src.frame->width || src.sws_src_h != src.frame->height || src.sws_fmt != src.frame->format) {
                        sws_freeContext(src.sws);
                        src.sws = sws_getContext(src.frame->width, src.frame->height, (AVPixelFormat)src.frame->format, width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                        src.sws_w = width; src.sws_h = height; src.sws_src_w = src.frame->width; src.sws_src_h = src.frame->height; src.sws_fmt = src.frame->format;
                    }
                    out_video->width = width; out_video->height = height; out_video->rgba.resize((size_t)width * height * 4);
                    std::uint8_t* planes[1] = {out_video->rgba.data()}; int strides[1] = {width * 4};
                    if (src.sws) sws_scale(src.sws, src.frame->data, src.frame->linesize, 0, src.frame->height, planes, strides);
                }
                av_frame_unref(src.frame); return true;
            }
            if (want_audio && audio_) push_audio(src, slot - 1, src.frame);
            av_frame_unref(src.frame);
        }
        if (src.drained) return false;
        if (src.eof) { avcodec_send_packet(src.video, nullptr); for (auto& t : src.tracks) if (t.ctx) avcodec_send_packet(t.ctx, nullptr); src.eof = false; src.drained = false; continue; }
        int r = av_read_frame(src.fmt, src.packet);
        if (r < 0) { src.eof = true; continue; }
        int sent = 0;
        if (src.packet->stream_index == src.vindex) sent = avcodec_send_packet(src.video, src.packet);
        else for (auto& t : src.tracks) if (t.ctx && src.packet->stream_index == t.index) { avcodec_send_packet(t.ctx, src.packet); break; }
        av_packet_unref(src.packet);
        if (sent < 0 && sent != AVERROR(EAGAIN)) { src.failed = true; return false; }
    }
}
void Player::work() {
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com; // Declared first so every WASAPI object is released before COM.
    Audio audio; if (SUCCEEDED(com.result) && audio.init()) audio_ = &audio;
    Source src; bool open_ok = false; size_t index = 0;
    std::shared_ptr<const std::vector<Span>> snapshot;
    auto publish_audio = [&] {
        if (fail_audio_for_test_.exchange(false) && audio_) audio_->failed = true;
        auto current = audio_ ? audio_->played_ms() : -1;
        if (audio_ && audio_->failed) {
            // Preserve the last published playhead; permanently use silence after
            // endpoint failure, so a pending device block cannot wedge decoding.
            auto at = position();
            clock_media_ = at; clock_wall_ = steady_ms();
            audio_->close(); audio_ = nullptr; src.reset_audio();
        }
        std::lock_guard clock_lock(audio_clock_mutex_);
        audio_time_ = audio_ ? current : -1;
    };
    std::unique_lock lock(mutex_);
    while (!stop_) {
        publish_audio();
        if (pending_seek_) {
            pending_seek_ = false; auto target = seek_target_.load(); auto gen = generation_.load(); snapshot = spans_;
            const auto& spans = *snapshot;
            bool resume = playing_;
            // A frame already decoded ahead satisfies short forward steps at once.
            auto queued = std::find_if(ready_.begin(), ready_.end(), [&](auto& f) { return std::llabs(f.ms - target) < 9; });
            if (!resume && queued != ready_.end()) {
                ready_.erase(ready_.begin(), queued); position_ = ready_.front().ms; seek_result_ = true; error_.clear();
                if (audio_) audio_->reset();
                { std::lock_guard clock_lock(audio_clock_mutex_); audio_time_ = -1; }
                restart_decoder_ = true; // Video can step from cache; audio must restart at this frame on Play.
                if (generation_ == gen) seeking_ = false;
                continue;
            }
            if (resume) ready_.clear();
            lock.unlock();
            if (audio_) audio_->reset();
            { std::lock_guard clock_lock(audio_clock_mutex_); audio_time_ = -1; }
            auto it = std::find_if(spans.begin(), spans.end(), [&](auto& s) { return target < s.end_ms; });
            std::string problem;
            if (it == spans.end() || target < it->start_ms - 1) problem = "No footage at the playhead.";
            else {
                index = (size_t)(it - spans.begin()); target = std::max(target, it->start_ms);
                // Continue decoding forward when the target is just ahead; reopen otherwise.
                bool same_file = open_ok && src.span.path == it->path;
                bool forward = same_file && src.last_video_ms >= 0 && target >= src.last_video_ms && target - src.last_video_ms < 3000;
                if (forward) src.reset_audio();
                // Same segment: seek in place and flush; another segment: reopen.
                if (!forward) open_ok = same_file ? reposition(src, target - it->start_ms) : open(src, *it, target - it->start_ms);
                if (!open_ok) problem = "Cannot decode this segment.";
                else {
                    Decoded frame; bool have = false;
                    for (int attempt = 0; attempt < 2 && !have; ++attempt) {
                        while (!stop_ && generation_ == gen && step(src, &frame, false)) {
                            if (frame.ms + 8 >= target) { have = true; break; }
                            // Show the frames on the way when paused, so the
                            // seek reads as motion toward the target.
                            if (!resume && frame.ms >= 0) { std::lock_guard guard(mutex_); if (generation_ != gen) break; ready_.clear(); ready_.push_back(std::move(frame)); seek_result_ = true; frame = Decoded(); }
                        }
                        // The native decoder could not use the device: reopen in software, once.
                        if (!have && src.failed && hw_ok_) { hw_ok_ = false; open_ok = open(src, *it, target - it->start_ms); if (!open_ok) break; } else break;
                    }
                    if (have) { std::lock_guard guard(mutex_); if (generation_ == gen) { ready_.clear(); ready_.push_back(std::move(frame)); position_ = ready_.back().ms; seek_result_ = true; } }
                    else if (generation_ == gen) problem = "No frame at the playhead.";
                }
            }
            lock.lock();
            if (generation_ != gen) continue;
            if (!problem.empty()) { error_ = problem; playing_ = false; position_ = seek_target_.load(); ready_.clear(); open_ok = false; } else error_.clear();
            if (resume && generation_ == gen && problem.empty()) { clock_media_ = position_.load(); clock_wall_ = steady_ms(); }
            if (generation_ == gen) seeking_ = false;
            continue;
        }
        const auto& spans = snapshot ? *snapshot : *spans_;
        if (!playing_) {
            if (audio_) audio_->suspend();
            publish_audio();
            resumable_ = open_ok && error_.empty();
            wake_.wait(lock); continue;
        }
        if (!open_ok) { playing_ = false; continue; }
        if (audio_) audio_->resume();
        publish_audio();
        if (drain_end_ms_ && src.pending.empty()) {
            if (audio_) audio_->start_tail();
            publish_audio();
            if (position() >= drain_end_ms_) {
                // Finish even when the window is hidden. Keep the last queued
                // picture for presentation when the viewport next repaints.
                while (ready_.size() > 1) ready_.pop_front();
                seek_result_ = !ready_.empty(); position_ = drain_end_ms_.load(); playing_ = false;
                if (audio_) audio_->suspend();
                continue;
            }
            space_.wait_for(lock, std::chrono::milliseconds(5)); continue;
        }
        // Playing: keep the video queue and the audio device fed. Both block
        // when full, which paces decoding at real time. A hidden or slow
        // viewport can miss pictures without stalling audio or the end stop.
        while (ready_.size() >= QueueFrames && ready_[1].ms <= position()) ready_.pop_front();
        if (ready_.size() >= QueueFrames) { space_.wait_for(lock, std::chrono::milliseconds(5)); continue; }
        auto gen = generation_.load(); auto end_limit = playback_end(); lock.unlock();
        bool progressed = false;
        if (audio_ && src.pending.size() > src.pending_offset) {
            UINT32 frames = (UINT32)((src.pending.size() - src.pending_offset) / audio_->format->nBlockAlign);
            // The device queue must never contain sound beyond the exclusive Out.
            if (end_limit && audio_->media_start >= 0) {
                auto remaining = av_rescale(end_limit - audio_->media_start, audio_->format->nSamplesPerSec, 1000) - audio_->written;
                frames = (UINT32)std::min<std::int64_t>(frames, std::max<std::int64_t>(0, remaining));
            }
            UINT32 n = audio_->write(src.pending.data() + src.pending_offset, frames);
            src.pending_offset += (size_t)n * audio_->format->nBlockAlign;
            if (!frames || src.pending_offset >= src.pending.size()) { src.pending.clear(); src.pending_offset = 0; }
            if (frames && !n) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); lock.lock(); continue; }
            progressed = true;
        }
        if (drain_end_ms_) { lock.lock(); continue; }
        auto drain = [&](std::int64_t end) {
            clock_media_ = position(); clock_wall_ = steady_ms(); drain_end_ms_ = end;
        };
        Decoded frame;
        if (src.pending.empty() && step(src, &frame, true)) {
            if (end_limit && frame.ms >= end_limit) { lock.lock(); if (generation_ == gen) drain(end_limit); continue; }
            lock.lock(); if (generation_ == gen) ready_.push_back(std::move(frame)); continue;
        } else if (src.pending.empty() && src.failed) {
            if (hw_ok_) { hw_ok_ = false; auto at = std::max<std::int64_t>(0, src.last_video_ms - src.span.start_ms); open_ok = open(src, src.span, at); lock.lock(); continue; }
            lock.lock(); error_ = "Cannot decode this segment."; playing_ = false; continue;
        } else if (src.pending.empty()) {
            // End of this segment: continue into the next one when contiguous.
            if ((!end_limit || src.span.end_ms < end_limit) && index + 1 < spans.size() && std::llabs(spans[index + 1].start_ms - src.span.end_ms) <= 1) {
                ++index; open_ok = open(src, spans[index], 0);
                lock.lock(); if (!open_ok) { error_ = "Cannot decode the next segment."; playing_ = false; } continue;
            }
            lock.lock(); if (generation_ == gen) drain(end_limit ? std::min(end_limit, src.span.end_ms) : src.span.end_ms); continue;
        }
        if (!progressed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        lock.lock();
    }
    audio_ = nullptr;
}
bool Player::convert_setup() {
    if (convert_ready_) return true;
    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* errors = nullptr;
    bool ok = SUCCEEDED(D3DCompile(ConvertShader, strlen(ConvertShader), "nv12", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vs, &errors)) &&
        SUCCEEDED(D3DCompile(ConvertShader, strlen(ConvertShader), "nv12", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ps, &errors)) &&
        SUCCEEDED(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) &&
        SUCCEEDED(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_));
    if (ok) {
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ok = SUCCEEDED(device_->CreateSamplerState(&sd, &sampler_));
    }
    if (vs) vs->Release(); if (ps) ps->Release(); if (errors) errors->Release();
    convert_ready_ = ok; return ok;
}
// Present one decoded frame: shader conversion from the decoder's NV12
// array slice into the RGBA target, or an upload for software frames.
void Player::present(const Decoded& frame) {
    bool gpu = frame.hw != nullptr;
    int width = gpu ? std::max(64, width_.load()) : frame.width;
    int height = gpu ? std::max(1, (int)std::lround((double)frame.height * width / std::max(1, frame.width))) : frame.height;
    if (!texture_ || tex_w_ != width || tex_h_ != height || tex_dynamic_ == gpu) {
        for (IUnknown** object : {(IUnknown**)&view_, (IUnknown**)&target_, (IUnknown**)&texture_}) if (*object) { (*object)->Release(); *object = nullptr; }
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = gpu ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (gpu ? D3D11_BIND_RENDER_TARGET : 0); desc.CPUAccessFlags = gpu ? 0 : D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture_))) return;
        device_->CreateShaderResourceView(texture_, nullptr, &view_);
        if (gpu) device_->CreateRenderTargetView(texture_, nullptr, &target_);
        tex_w_ = width; tex_h_ = height; tex_dynamic_ = !gpu;
    }
    if (!gpu) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(texture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            for (int y = 0; y < height; ++y) memcpy((std::uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch, frame.rgba.data() + (size_t)y * width * 4, (size_t)width * 4);
            context_->Unmap(texture_, 0);
        }
        return;
    }
    if (!convert_setup() || !target_) return;
    auto* source = reinterpret_cast<ID3D11Texture2D*>(frame.hw->data[0]); UINT slice = (UINT)(intptr_t)frame.hw->data[1];
    ID3D11ShaderResourceView* planes[2] = {};
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = 1; sv.Texture2DArray.FirstArraySlice = slice; sv.Texture2DArray.ArraySize = 1;
    sv.Format = DXGI_FORMAT_R8_UNORM; device_->CreateShaderResourceView(source, &sv, &planes[0]);
    sv.Format = DXGI_FORMAT_R8G8_UNORM; device_->CreateShaderResourceView(source, &sv, &planes[1]);
    if (planes[0] && planes[1]) {
        D3D11_VIEWPORT viewport{0, 0, (float)width, (float)height, 0, 1};
        context_->OMSetRenderTargets(1, &target_, nullptr); context_->RSSetViewports(1, &viewport);
        context_->IASetInputLayout(nullptr); context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_, nullptr, 0); context_->PSSetShader(ps_, nullptr, 0);
        context_->PSSetShaderResources(0, 2, planes); context_->PSSetSamplers(0, 1, &sampler_);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {}; context_->PSSetShaderResources(0, 2, none);
        ID3D11RenderTargetView* no_target = nullptr; context_->OMSetRenderTargets(1, &no_target, nullptr);
    }
    for (auto* plane : planes) if (plane) plane->Release();
}
Player::Picture Player::tick() {
    std::unique_lock lock(mutex_);
    std::optional<Decoded> next;
    if (!error_.empty()) { lock.unlock(); shown_.reset(); return {}; }
    if (!playing_) {
        // Paused: only a seek result replaces the picture; queued frames stay for resume.
        if (seek_result_ && !ready_.empty()) { next = ready_.front(); seek_result_ = false; }
    } else {
        auto now = position(), end = playback_end();
        while (!ready_.empty() && ready_.front().ms <= now && (!end || ready_.front().ms < end)) { next = std::move(ready_.front()); ready_.pop_front(); }
        if (next) seek_result_ = false;
    }
    lock.unlock(); space_.notify_all();
    if (next) { present(*next); next->hw.reset(); next->rgba.clear(); shown_ = std::move(next); }
    if (!shown_ || !view_) return {};
    return {(ImTextureID)view_, tex_w_, tex_h_, shown_->ms};
}
}

#include "thumbs.hpp"
#include <sstream>
namespace clip {
// Off-screen D3D device, then: keyframe and exact decodes through the
// thumbnail path, a cold seek, playback for a few seconds, pause/resume, and
// a short forward step. Results go to player-test.txt in the app directory.
int player_test(const fs::path& buffer_root, int play_seconds) {
    std::ostringstream report; auto ms = [] { return steady_ms(); };
    ID3D11Device* device = nullptr; ID3D11DeviceContext* context = nullptr; D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context))) {
        atomic_write(app_dir() / "player-test.txt", "Cannot create a D3D11 device\n"); return 1;
    }
    int failures = 0;
    {
        auto map = scan_buffer(buffer_root);
        report << "segments: " << map.spans.size() << "\n";
        if (map.spans.empty()) { atomic_write(app_dir() / "player-test.txt", report.str() + "No closed segments\n"); return 1; }
        // Start in a segment that has a contiguous successor, a few from the end.
        size_t pick = map.spans.size() > 8 ? map.spans.size() - 8 : 0;
        while (pick + 1 < map.spans.size() && std::llabs(map.spans[pick + 1].start_ms - map.spans[pick].end_ms) > 1) ++pick;
        auto& last = map.spans[pick];
        auto priv = hw_device(nullptr);
        report << "render device: " << adapter_name(device) << "\n";
        report << "FFmpeg private device: " << (priv ? adapter_name(*priv) : "none") << "\n";
        auto hw = hw_device(device);
        report << "thumbnail device: " << (hw ? "D3D11VA on " + adapter_name(*hw) : "software") << "\n";
        Decoder decoder(hw);
        for (size_t i = 0; i < std::min<size_t>(3, map.spans.size() - pick); ++i) {
            auto t = ms(); decoder.decode(map.spans[pick + i].path, 1500, 320, true); auto key = ms() - t;
            t = ms(); decoder.decode(map.spans[pick + i].path, 1500, 320, false); auto exact = ms() - t;
            report << "decode keyframe " << key << " ms, exact frame at +1.5 s " << exact << " ms" << (i == 0 ? " (first call includes decoder setup)" : "") << "\n";
        }
        { Decoder software; auto t = ms(); software.decode(last.path, 1500, 320, false); report << "software exact frame at +1.5 s: " << ms() - t << " ms\n"; }
        // Raw decode throughput: 120 frames straight through, no scaling.
        for (int variant = 0; variant < 3; ++variant) {
            AVFormatContext* fmt = nullptr; if (avformat_open_input(&fmt, path_text(last.path).c_str(), nullptr, nullptr) < 0) break;
            avformat_find_stream_info(fmt, nullptr); int vindex = -1;
            for (unsigned i = 0; i < fmt->nb_streams; ++i) if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vindex = (int)i; break; }
            bool use_hw = variant < 2;
            const AVCodec* codec = pick_decoder(fmt->streams[vindex]->codecpar->codec_id, use_hw);
            AVCodecContext* ctx = avcodec_alloc_context3(codec); avcodec_parameters_to_context(ctx, fmt->streams[vindex]->codecpar);
            HwBinding binding{hw, false, 16};
            ctx->thread_count = variant == 1 ? 0 : (variant == 0 ? 1 : 0);
            if (use_hw) { ctx->opaque = &binding; ctx->get_format = hw_get_format; }
            avcodec_open2(ctx, codec, nullptr);
            AVPacket* packet = av_packet_alloc(); AVFrame* frame = av_frame_alloc();
            int frames = 0; double send_ms = 0, receive_ms = 0, wait_max = 0; auto t = ms(); bool hw_frames = false;
            while (frames < 120 && av_read_frame(fmt, packet) >= 0) {
                if (packet->stream_index == vindex) {
                    auto a = std::chrono::steady_clock::now(); avcodec_send_packet(ctx, packet);
                    auto b = std::chrono::steady_clock::now(); send_ms += std::chrono::duration<double, std::milli>(b - a).count();
                    while (avcodec_receive_frame(ctx, frame) >= 0) {
                        auto c = std::chrono::steady_clock::now(); double d = std::chrono::duration<double, std::milli>(c - b).count();
                        receive_ms += d; wait_max = std::max(wait_max, d); b = c;
                        hw_frames = frame->format == AV_PIX_FMT_D3D11; ++frames; av_frame_unref(frame);
                    }
                }
                av_packet_unref(packet);
            }
            report << (use_hw ? (variant == 0 ? "hw threads=1" : "hw threads=auto") : "software threads=auto") << ": " << frames << " frames in " << ms() - t
                << " ms (send " << (int)send_ms << " ms, receive " << (int)receive_ms << " ms, longest receive " << (int)wait_max << " ms, hw frames " << hw_frames << ")\n";
            av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&ctx); avformat_close_input(&fmt);
        }
        Player player(device, context); player.set_spans(map.spans); player.set_end(map.last_end_ms); player.set_width(960);
        report << "player device: " << (player.hardware() ? "D3D11VA" : "software") << "\n";
        std::int64_t previous_ms = 0;
        auto wait_seek = [&](std::int64_t target) {
            previous_ms = player.tick().ms;
            auto t = ms(); player.seek(target); Player::Picture picture;
            // Done when the player has landed and presented the frame, or on
            // timeout. Frames shown on the way do not count.
            while (ms() - t < 10000) {
                picture = player.tick();
                if (!player.seeking() && picture.texture && std::llabs(picture.ms - target) < 9) break;
                if (!player.busy() && !player.error().empty()) { picture = {}; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            previous_ms = picture.ms;
            return std::pair{ms() - t, picture};
        };
        std::int64_t start = last.start_ms + 1500;
        auto [cold_ms, cold] = wait_seek(start);
        report << "cold seek " << cold_ms << " ms, frame " << (cold.texture ? local_time(cold.ms, true) : "none") << ", error '" << player.error() << "'\n";
        if (!cold.texture) ++failures;
        player.play(); auto t = ms(); int presented = 0; std::int64_t last_ms = 0; Player::Picture picture;
        while (ms() - t < play_seconds * 1000) { picture = player.tick(); if (picture.ms != last_ms) { ++presented; last_ms = picture.ms; } std::this_thread::sleep_for(std::chrono::milliseconds(4)); }
        previous_ms = picture.ms;
        auto advanced = picture.ms - start;
        report << "play " << play_seconds << " s: presented " << presented << " distinct frames, media advanced " << advanced << " ms, playing=" << player.playing() << ", error '" << player.error() << "'\n";
        report << "clock minus presented frame: " << player.position() - picture.ms << " ms (audio clock leads video when positive)\n";
        if (presented < play_seconds * 30 || advanced < play_seconds * 800) ++failures;
        player.pause(); auto paused_at = player.position(); std::this_thread::sleep_for(std::chrono::milliseconds(300));
        t = ms(); player.play(); Player::Picture resumed; for (int i = 0; i < 500 && (resumed.ms <= paused_at); ++i) { resumed = player.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        report << "resume after pause: first new frame after " << ms() - t << " ms\n"; player.pause();
        auto at = player.position(); report << "paused at " << local_time(at, true) << ", presented " << local_time(player.tick().ms, true) << "\n";
        auto [step_ms, stepped] = wait_seek(at + 17);
        report << "step forward one frame to " << local_time(at + 17, true) << ": " << step_ms << " ms, frame " << (stepped.texture ? local_time(stepped.ms, true) : "none") << "\n";
        auto [back_ms, back] = wait_seek(at - 1000);
        report << "seek back one second to " << local_time(at - 1000, true) << ": " << back_ms << " ms, frame " << (back.texture ? local_time(back.ms, true) : "none") << "\n";
        if (!stepped.texture || !back.texture) ++failures;
        // Exercise the real decoder, presentation clock and device queue at
        // clip boundaries, including a one-frame clip and a segment seam.
        auto check = [&](bool ok, const char* name) { report << (ok ? "PASS " : "FAIL ") << name << '\n'; if (!ok) ++failures; };
        auto run_range = [&](std::int64_t in, std::int64_t out, std::int64_t from, const char* name) {
            player.pause(); player.set_range(in, out); wait_seek(from);
            auto began = ms(); player.play();
            bool within = true; std::int64_t first = 0; Player::Picture final;
            while (player.busy() && ms() - began < 5000) {
                final = player.tick();
                if (!player.seeking() && final.texture) {
                    if (!first) first = final.ms;
                    within &= final.ms >= in && final.ms < out;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            final = player.tick();
            auto expected_start = from < in || from >= out ? in : from;
            auto elapsed = ms() - began;
            report << name << ": elapsed " << elapsed << " ms, first " << first - in << ", last " << final.ms - out << " ms from Out\n";
            check(!player.playing() && player.error().empty() && within && first >= expected_start && first < expected_start + 100 &&
                final.texture && final.ms < out && final.ms >= out - 18 && player.position() == out && elapsed >= out - expected_start - 50, name);
        };
        // Supersede both queued and in-flight seeks; only the final request may land.
        player.pause(); player.set_range(0, 0);
        for (int i = 0; i < 24; ++i) {
            player.seek(last.start_ms + 500 + (i % 5) * 170);
            if (i % 3 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto [superseded_ms, superseded] = wait_seek(last.start_ms + 1250);
        check(!player.seeking() && superseded.texture && std::llabs(superseded.ms - (last.start_ms + 1250)) < 9 &&
            std::llabs(player.position() - (last.start_ms + 1250)) < 9, "superseding seeks publish only the final target");
        auto clip_in = last.start_ms + 1000, clip_out = clip_in + 1000;
        run_range(clip_in, clip_out, clip_in - 500, "play before In restarts at In and drains to Out");
        run_range(clip_in, clip_out, clip_out + 500, "play after Out restarts at In");
        run_range(clip_in, clip_out, clip_in + 500, "play inside resumes to Out");
        // Replay directly from the stopped boundary, without a seek.
        player.play(); check(player.playing() && std::llabs(player.position() - clip_in) < 100, "Play at Out restarts the clip");
        auto range_began = ms(); while (player.seeking() && ms() - range_began < 5000) { player.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        player.pause(); auto parked = player.position();
        player.set_range(clip_in + 100, clip_out - 100);
        check(player.position() == parked && !player.playing(), "editing the range preserves the playhead");
        auto [outside_ms, outside] = wait_seek(clip_out + 500);
        check(outside.texture && std::llabs(player.position() - (clip_out + 500)) < 9, "inspection can seek outside the range");
        player.set_range(clip_in, 0); player.play();
        check(player.position() > clip_out, "an incomplete range leaves buffer playback unrestricted"); player.pause();
        player.set_range(0, clip_out); player.play();
        check(player.position() > clip_out, "Out alone leaves buffer playback unrestricted"); player.pause();
        run_range(clip_in, clip_in + 17, clip_in - 500, "one-frame clip holds its only included frame");
        player.set_range(clip_in, clip_out); wait_seek(clip_in); player.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        auto hidden_tail = player.tick();
        check(!player.playing() && player.position() == clip_out && hidden_tail.ms >= clip_out - 18 && hidden_tail.ms < clip_out, "clip drains to its last frame without viewport ticks");
        run_range(last.end_ms - 500, last.end_ms, last.end_ms - 500, "Out at a segment seam excludes the next segment");
        if (pick + 1 < map.spans.size() && std::llabs(map.spans[pick + 1].start_ms - last.end_ms) <= 1)
            run_range(last.end_ms - 500, last.end_ms + 500, last.end_ms - 500, "clip plays across a segment seam");
        player.set_range(0, 0); wait_seek(clip_in); player.play();
        range_began = ms(); while (player.playing() && player.position() <= clip_out + 150 && ms() - range_began < 3000) { player.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        check(player.playing() && player.position() > clip_out, "Clear restores playback beyond the old Out"); player.pause();
        player.set_range(0, 0); wait_seek(map.last_end_ms - 500); player.play();
        range_began = ms(); while (player.busy() && ms() - range_began < 3000) { player.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        auto tail = player.tick();
        check(!player.playing() && tail.texture && tail.ms >= map.last_end_ms - 18 && tail.ms < map.last_end_ms, "buffer playback drains the last closed segment");
        auto first_cut = decoder.decode(last.path, clip_in - last.start_ms, 320, false);
        auto last_cut = decoder.decode(last.path, last.end_ms - last.start_ms - 1, 320, false);
        check(std::llabs(first_cut.pts_ms - (clip_in - last.start_ms)) < 1 && last_cut.pts_ms < last.end_ms - last.start_ms &&
            last_cut.pts_ms >= last.end_ms - last.start_ms - 18, "trim previews decode In and the last included frame before Out");
        // Inject the same sticky error state used for failed WASAPI calls.
        player.set_range(clip_in, clip_out); wait_seek(clip_in); player.play();
        auto failure_began = ms();
        while (player.seeking() && ms() - failure_began < 3000) {
            player.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        failure_began = ms();
        while (player.playing() && ms() - failure_began < 200) {
            player.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        bool had_audio_clock = false;
        {
            std::lock_guard clock_lock(player.audio_clock_mutex_);
            had_audio_clock = player.audio_time_ >= 0;
            report << "endpoint failure injection: " << (player.audio_time_ >= 0 ? "active audio clock" : "audio unavailable or not started; silent path only") << '\n';
        }
        auto before_failure = player.position();
        player.fail_audio_for_test_ = true;
        bool continuous = true; auto observed = before_failure;
        failure_began = ms();
        while (player.playing() && ms() - failure_began < 3000) {
            auto current = player.position();
            continuous &= current >= observed - 20; observed = current;
            player.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (had_audio_clock) {
            bool fell_back;
            { std::lock_guard clock_lock(player.audio_clock_mutex_); fell_back = player.audio_time_ < 0; }
            check(fell_back && continuous && !player.playing() && player.position() == clip_out && player.error().empty(),
                "endpoint failure switches to continuous silent playback and drains");
        } else report << "SKIP endpoint failure recovery: no active audio endpoint\n";
        player.set_range(0, 0);
        auto thumbnail_failure = thumbnail_test(device, last.path);
        check(thumbnail_failure.empty(), "thumbnail cancellation, ownership and memory bounds");
        if (!thumbnail_failure.empty()) report << thumbnail_failure << '\n';
        // Thumbnail cache: tiles at two spacings over three segments resolve
        // to the same keyframes, so the second pass must decode nothing.
        {
            Thumbnails thumbs(device);
            auto tiles = [&](std::int64_t tile_ms) {
                int missing = 0;
                for (int pass = 0; pass < 600; ++pass) {
                    missing = 0;
                    for (size_t i = pick; i < std::min(map.spans.size(), pick + 3); ++i)
                        for (std::int64_t t = map.spans[i].start_ms / tile_ms * tile_ms; t < map.spans[i].end_ms; t += tile_ms) {
                            if (t < map.spans[i].start_ms) continue;
                            if (!thumbs.keyframe(map.spans[i].path, t - map.spans[i].start_ms, 160).texture) ++missing;
                        }
                    thumbs.tick(); if (!missing) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return missing;
            };
            auto began = ms(); int first_missing = tiles(700); auto first_ms = ms() - began; auto first_decodes = thumbs.decodes();
            began = ms(); int second_missing = tiles(1300); auto second_ms = ms() - began; auto second_decodes = thumbs.decodes() - first_decodes;
            report << "thumbnails: first spacing " << first_decodes << " decodes in " << first_ms << " ms, second spacing " << second_decodes
                << " decodes in " << second_ms << " ms, " << thumbs.cached() << " cached\n";
            if (first_missing || second_missing || second_decodes) ++failures;
        }
    }
    context->Release(); device->Release();
    report << (failures ? "FAIL\n" : "PASS\n");
    atomic_write(app_dir() / "player-test.txt", report.str());
    return failures ? 1 : 0;
}
}
