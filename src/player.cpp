#include "player.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace clip {
namespace {
std::int64_t steady_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
constexpr size_t QueueFrames = 24;       // ~400 ms of decoded video ahead of the playhead.
constexpr REFERENCE_TIME AudioBuffer = 5000000; // 500 ms shared-mode render buffer.
}

// Shared-mode WASAPI output in the device's mix format; decoded audio is
// converted to it with swresample.
struct Player::Audio {
    IAudioClient* client = nullptr; IAudioRenderClient* render = nullptr; WAVEFORMATEX* format = nullptr;
    UINT32 buffer_frames = 0; bool started = false, is_float = false; std::int64_t written = 0, media_start = -1;
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
    void reset() { if (client) { client->Stop(); client->Reset(); } started = false; written = 0; media_start = -1; }
    // Frames of the pending block that fit now; 0 when the device buffer is full.
    UINT32 write(const std::uint8_t* data, UINT32 frames) {
        UINT32 padding = 0; if (FAILED(client->GetCurrentPadding(&padding))) return 0;
        UINT32 n = std::min(frames, buffer_frames - padding); if (!n) return 0;
        BYTE* out = nullptr; if (FAILED(render->GetBuffer(n, &out))) return 0;
        memcpy(out, data, (size_t)n * format->nBlockAlign); render->ReleaseBuffer(n, 0); written += n;
        if (!started && written >= (std::int64_t)buffer_frames / 4) { client->Start(); started = true; }
        return n;
    }
    std::int64_t played_ms() const {
        if (!started || media_start < 0) return -1;
        UINT32 padding = 0; client->GetCurrentPadding(&padding);
        return media_start + (written - (std::int64_t)padding) * 1000 / format->nSamplesPerSec;
    }
    ~Audio() { if (client) client->Stop(); if (render) render->Release(); if (client) client->Release(); if (format) CoTaskMemFree(format); }
};

struct Player::Source {
    AVFormatContext* fmt = nullptr; AVCodecContext* video = nullptr; AVCodecContext* audio = nullptr;
    AVPacket* packet = av_packet_alloc(); AVFrame* frame = av_frame_alloc();
    SwsContext* sws = nullptr; SwrContext* swr = nullptr; int sws_w = 0, sws_h = 0, sws_src_w = 0, sws_src_h = 0, sws_fmt = -1;
    int vindex = -1, aindex = -1; Span span; bool eof = false, drained = false;
    std::int64_t last_video_ms = -1;
    std::vector<std::uint8_t> pending; // Converted audio not yet accepted by the device.
    size_t pending_offset = 0;
    void close() {
        sws_freeContext(sws); sws = nullptr; swr_free(&swr);
        avcodec_free_context(&video); avcodec_free_context(&audio); avformat_close_input(&fmt);
        vindex = aindex = -1; eof = drained = false; last_video_ms = -1; pending.clear(); pending_offset = 0;
    }
    ~Source() { close(); av_packet_free(&packet); av_frame_free(&frame); }
    std::int64_t ms(AVFrame* f, int index) const {
        std::int64_t pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
        return pts == AV_NOPTS_VALUE ? -1 : span.start_ms + av_rescale_q(pts, fmt->streams[index]->time_base, AVRational{1, 1000});
    }
};

Player::Player(ID3D11Device* device, ID3D11DeviceContext* context) : device_(device), context_(context), worker_([this] { work(); }) {}
Player::~Player() {
    stop_ = true; wake_.notify_all(); space_.notify_all(); worker_.join();
    if (view_) view_->Release(); if (texture_) texture_->Release();
}
void Player::set_spans(std::vector<Span> spans) { std::lock_guard lock(mutex_); spans_ = std::move(spans); }
void Player::seek(std::int64_t ms) {
    seek_target_ = ms; pending_seek_ = true; playing_ = false; ++generation_;
    wake_.notify_all(); space_.notify_all();
}
void Player::play() {
    if (playing_) return;
    seek_target_ = position_.load(); clock_media_ = position_.load(); clock_wall_ = steady_ms();
    pending_seek_ = true; playing_ = true; ++generation_;
    wake_.notify_all(); space_.notify_all();
}
void Player::pause() {
    if (!playing_) return;
    position_ = position(); playing_ = false; ++generation_; space_.notify_all();
}
std::int64_t Player::position() const {
    if (pending_seek_) return seek_target_;
    if (!playing_) return position_;
    if (audio_) { auto ms = audio_->played_ms(); if (ms >= 0) return ms; }
    return clock_media_ + (steady_ms() - clock_wall_);
}
bool Player::open(Source& src, const Span& span, std::int64_t offset_ms) {
    src.close(); src.span = span;
    if (avformat_open_input(&src.fmt, path_text(span.path).c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(src.fmt, nullptr) < 0) return false;
    for (unsigned i = 0; i < src.fmt->nb_streams; ++i) {
        auto* par = src.fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && src.vindex < 0) src.vindex = (int)i;
        if (par->codec_type == AVMEDIA_TYPE_AUDIO && src.aindex < 0) src.aindex = (int)i;
    }
    if (src.vindex < 0) return false;
    auto make = [&](int index, AVCodecContext*& ctx) {
        auto* par = src.fmt->streams[index]->codecpar; const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec || !(ctx = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(ctx, par) < 0) return false;
        ctx->thread_count = 0; return avcodec_open2(ctx, codec, nullptr) >= 0;
    };
    if (!make(src.vindex, src.video)) return false;
    if (src.aindex >= 0 && audio_ && !make(src.aindex, src.audio)) { avcodec_free_context(&src.audio); src.aindex = -1; }
    if (src.audio) {
        AVChannelLayout out{}; av_channel_layout_default(&out, audio_->format->nChannels);
        if (swr_alloc_set_opts2(&src.swr, &out, audio_->is_float ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16, (int)audio_->format->nSamplesPerSec,
                &src.audio->ch_layout, (AVSampleFormat)src.audio->sample_fmt, src.audio->sample_rate, 0, nullptr) < 0 || swr_init(src.swr) < 0) {
            swr_free(&src.swr); avcodec_free_context(&src.audio); src.aindex = -1;
        }
        av_channel_layout_uninit(&out);
    }
    if (offset_ms > 0) {
        auto target = av_rescale_q(offset_ms, AVRational{1, 1000}, src.fmt->streams[src.vindex]->time_base);
        av_seek_frame(src.fmt, src.vindex, target, AVSEEK_FLAG_BACKWARD);
    }
    return true;
}
void Player::push_audio(Source& src, void* f) {
    auto* frame = static_cast<AVFrame*>(f);
    int out_samples = swr_get_out_samples(src.swr, frame->nb_samples); if (out_samples <= 0) return;
    size_t block = audio_->format->nBlockAlign; size_t old = src.pending.size();
    src.pending.resize(old + (size_t)out_samples * block);
    std::uint8_t* out = src.pending.data() + old;
    int got = swr_convert(src.swr, &out, out_samples, (const std::uint8_t**)frame->data, frame->nb_samples);
    src.pending.resize(old + (size_t)std::max(0, got) * block);
    if (audio_->media_start < 0) audio_->media_start = src.ms(frame, src.aindex);
}
// Decode until one video frame is produced (returned in out_video) or the
// file ends. Audio frames are converted and queued along the way.
bool Player::step(Source& src, Decoded* out_video, bool want_audio) {
    while (true) {
        for (auto* ctx : {src.video, src.audio}) {
            if (!ctx) continue;
            int r = avcodec_receive_frame(ctx, src.frame);
            if (r < 0) { if (r == AVERROR_EOF && ctx == src.video) src.drained = true; continue; }
            if (ctx == src.video) {
                std::int64_t ms = src.ms(src.frame, src.vindex); src.last_video_ms = ms;
                int width = std::max(64, width_.load()), height = std::max(1, (int)std::lround((double)src.frame->height * width / std::max(1, src.frame->width)));
                if (!src.sws || src.sws_w != width || src.sws_src_w != src.frame->width || src.sws_src_h != src.frame->height || src.sws_fmt != src.frame->format) {
                    sws_freeContext(src.sws);
                    src.sws = sws_getContext(src.frame->width, src.frame->height, (AVPixelFormat)src.frame->format, width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    src.sws_w = width; src.sws_h = height; src.sws_src_w = src.frame->width; src.sws_src_h = src.frame->height; src.sws_fmt = src.frame->format;
                }
                out_video->ms = ms; out_video->width = width; out_video->height = height; out_video->rgba.resize((size_t)width * height * 4);
                std::uint8_t* planes[1] = {out_video->rgba.data()}; int strides[1] = {width * 4};
                if (src.sws) sws_scale(src.sws, src.frame->data, src.frame->linesize, 0, src.frame->height, planes, strides);
                av_frame_unref(src.frame); return true;
            }
            if (want_audio && audio_) push_audio(src, src.frame);
            av_frame_unref(src.frame);
        }
        if (src.drained) return false;
        if (src.eof) { avcodec_send_packet(src.video, nullptr); if (src.audio) avcodec_send_packet(src.audio, nullptr); src.eof = false; src.drained = false; continue; }
        int r = av_read_frame(src.fmt, src.packet);
        if (r < 0) { src.eof = true; continue; }
        if (src.packet->stream_index == src.vindex) avcodec_send_packet(src.video, src.packet);
        else if (src.packet->stream_index == src.aindex && src.audio) avcodec_send_packet(src.audio, src.packet);
        av_packet_unref(src.packet);
    }
}
void Player::work() {
    Audio audio; if (audio.init()) audio_ = &audio;
    Source src; bool open_ok = false; size_t index = 0; std::vector<Span> spans;
    std::unique_lock lock(mutex_);
    while (!stop_) {
        if (pending_seek_) {
            pending_seek_ = false; auto target = seek_target_.load(); auto gen = generation_.load(); spans = spans_;
            bool resume = playing_; lock.unlock();
            if (audio_) audio_->reset();
            auto it = std::find_if(spans.begin(), spans.end(), [&](auto& s) { return target < s.end_ms; });
            std::string problem;
            if (it == spans.end() || target < it->start_ms - 1) problem = "No footage at the playhead.";
            else {
                index = (size_t)(it - spans.begin()); target = std::max(target, it->start_ms);
                // Continue decoding forward when the target is just ahead; reopen otherwise.
                bool forward = open_ok && src.span.path == it->path && src.last_video_ms >= 0 && target >= src.last_video_ms && target - src.last_video_ms < 3000;
                if (!forward) open_ok = open(src, *it, target - it->start_ms);
                if (!open_ok) problem = "Cannot decode this segment.";
                else {
                    Decoded frame; bool have = false;
                    while (!stop_ && generation_ == gen && step(src, &frame, false)) { if (frame.ms + 8 >= target) { have = true; break; } }
                    if (have) { std::lock_guard guard(mutex_); ready_.clear(); ready_.push_back(std::move(frame)); position_ = ready_.back().ms; }
                    else if (generation_ == gen) problem = "No frame at the playhead.";
                }
            }
            lock.lock();
            if (!problem.empty()) { error_ = problem; playing_ = false; position_ = seek_target_.load(); ready_.clear(); } else error_.clear();
            if (resume && generation_ == gen && problem.empty()) { clock_media_ = position_.load(); clock_wall_ = steady_ms(); }
            continue;
        }
        if (!playing_) { if (audio_ && audio_->started) audio_->reset(); wake_.wait(lock); continue; }
        if (!open_ok) { playing_ = false; continue; }
        // Playing: keep the video queue and the audio device fed. Both block
        // when full, which paces decoding at real time.
        if (ready_.size() >= QueueFrames) { space_.wait_for(lock, std::chrono::milliseconds(5)); continue; }
        auto gen = generation_.load(); auto end_limit = end_ms_.load(); lock.unlock();
        bool progressed = false;
        if (audio_ && src.pending.size() > src.pending_offset) {
            UINT32 frames = (UINT32)((src.pending.size() - src.pending_offset) / audio_->format->nBlockAlign);
            UINT32 n = audio_->write(src.pending.data() + src.pending_offset, frames);
            src.pending_offset += (size_t)n * audio_->format->nBlockAlign;
            if (src.pending_offset >= src.pending.size()) { src.pending.clear(); src.pending_offset = 0; }
            if (!n) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); lock.lock(); continue; }
            progressed = true;
        }
        Decoded frame;
        if (src.pending.empty() && step(src, &frame, true)) {
            if (end_limit && frame.ms >= end_limit) { lock.lock(); playing_ = false; position_ = frame.ms; continue; }
            lock.lock(); if (generation_ == gen) ready_.push_back(std::move(frame)); continue;
        } else if (src.pending.empty()) {
            // End of this segment: continue into the next one when contiguous.
            if (index + 1 < spans.size() && spans[index + 1].start_ms - src.span.end_ms < 400) {
                ++index; open_ok = open(src, spans[index], 0);
                lock.lock(); if (!open_ok) { error_ = "Cannot decode the next segment."; playing_ = false; } continue;
            }
            lock.lock(); playing_ = false; position_ = src.last_video_ms > 0 ? src.last_video_ms : position_.load(); continue;
        }
        if (!progressed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        lock.lock();
    }
    audio_ = nullptr;
}
Player::Picture Player::tick() {
    std::unique_lock lock(mutex_);
    std::optional<Decoded> next;
    if (!error_.empty()) { lock.unlock(); shown_.reset(); return {}; }
    if (!playing_) { if (!ready_.empty()) { next = std::move(ready_.front()); ready_.clear(); } }
    else {
        auto now = position();
        while (!ready_.empty() && ready_.front().ms <= now) { next = std::move(ready_.front()); ready_.pop_front(); }
        // Without audio the wall clock drives playback; with audio the device does.
        if (next && !(audio_ && audio_->played_ms() >= 0)) { clock_media_ = next->ms; clock_wall_ = steady_ms(); }
    }
    lock.unlock(); space_.notify_all();
    if (next) {
        if (!texture_ || tex_w_ != next->width || tex_h_ != next->height) {
            if (view_) view_->Release(); if (texture_) texture_->Release(); view_ = nullptr; texture_ = nullptr;
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = next->width; desc.Height = next->height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &texture_))) { device_->CreateShaderResourceView(texture_, nullptr, &view_); tex_w_ = next->width; tex_h_ = next->height; }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (texture_ && SUCCEEDED(context_->Map(texture_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            for (int y = 0; y < next->height; ++y) memcpy((std::uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch, next->rgba.data() + (size_t)y * next->width * 4, (size_t)next->width * 4);
            context_->Unmap(texture_, 0);
        }
        shown_ = std::move(next);
    }
    if (!shown_ || !view_) return {};
    return {(ImTextureID)view_, shown_->width, shown_->height, shown_->ms};
}
}
