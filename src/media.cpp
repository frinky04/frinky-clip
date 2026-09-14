#include "media.hpp"
#include <d3d11.h>
#include <dxgi.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/error.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace clip {
void av_check(int code, const char* context) {
    if (code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, message, sizeof(message));
    throw std::runtime_error(std::string(context) + ": " + message);
}
// True when every stream's header already carries what decoding needs, so the
// expensive probe pass can be skipped. Our own MKV segments always do.
static bool headers_complete(AVFormatContext* fmt) {
    if (!fmt->nb_streams) return false;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        auto* par = fmt->streams[i]->codecpar;
        if (par->codec_id == AV_CODEC_ID_NONE || fmt->streams[i]->time_base.den <= 0) return false;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && (par->width <= 0 || par->height <= 0)) return false;
        if (par->codec_type == AVMEDIA_TYPE_AUDIO && (par->sample_rate <= 0 || par->ch_layout.nb_channels <= 0)) return false;
    }
    return true;
}
struct Input {
    AVFormatContext* value = nullptr;
    // `probe` forces the stream-info pass, which inspect_media needs for durations.
    explicit Input(const fs::path& p, bool probe = true) {
        av_check(avformat_open_input(&value, path_text(p).c_str(), nullptr, nullptr), "Open recording");
        if (probe || !headers_complete(value)) {
            int result = avformat_find_stream_info(value, nullptr);
            if (result < 0) { avformat_close_input(&value); av_check(result, "Read recording streams"); }
        }
    }
    ~Input() { avformat_close_input(&value); }
};
bool open_without_probe(AVFormatContext** fmt, const fs::path& p) {
    if (avformat_open_input(fmt, path_text(p).c_str(), nullptr, nullptr) < 0) return false;
    if (headers_complete(*fmt)) return true;
    if (avformat_find_stream_info(*fmt, nullptr) < 0) { avformat_close_input(fmt); return false; }
    return true;
}
MediaInfo inspect_media(const fs::path& p) {
    Input in(p); MediaInfo info;
    if (in.value->duration != AV_NOPTS_VALUE) info.seconds = double(in.value->duration) / AV_TIME_BASE;
    for (unsigned i = 0; i < in.value->nb_streams; ++i) {
        auto* par = in.value->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) { info.width = par->width; info.height = par->height; }
        if (par->codec_type == AVMEDIA_TYPE_AUDIO) info.audio = true;
    }
    if (!info.width || !std::isfinite(info.seconds) || info.seconds <= 0) throw std::runtime_error("Recording has no complete video duration: " + path_text(p));
    return info;
}
VideoExtent probe_video(const fs::path& p) {
    Input in(p, false); int index = -1;
    for (unsigned i = 0; i < in.value->nb_streams; ++i) if (in.value->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { index = (int)i; break; }
    if (index < 0) throw std::runtime_error("No video stream: " + path_text(p));
    auto* stream = in.value->streams[index]; VideoExtent extent;
    AVRational rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    AVPacket* packet = av_packet_alloc(); std::int64_t first = AV_NOPTS_VALUE, last = AV_NOPTS_VALUE;
    int result;
    while ((result = av_read_frame(in.value, packet)) >= 0) {
        if (packet->stream_index == index) {
            ++extent.frames;
            if (packet->pts != AV_NOPTS_VALUE) { if (first == AV_NOPTS_VALUE) first = packet->pts; last = packet->pts; }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (result != AVERROR_EOF) av_check(result, "Read segment");
    if (!extent.frames) throw std::runtime_error("Recording has no video frames: " + path_text(p));
    if (rate.num > 0 && rate.den > 0) { extent.fps_num = rate.num; extent.fps_den = rate.den; }
    else if (extent.frames > 1 && first != AV_NOPTS_VALUE && last > first) {
        // No declared rate: derive it from the packet spacing.
        auto span_us = av_rescale_q(last - first, stream->time_base, AVRational{1, 1000000});
        extent.fps_num = (int)std::lround((extent.frames - 1) * 1000000.0 / (double)span_us); extent.fps_den = 1;
    }
    return extent;
}
std::vector<AudioLevels> audio_levels(const fs::path& p, int bin_ms) {
    Input in(p, false);
    // One decoder per audio stream; every packet goes to its stream's track.
    struct Track {
        int index; AVCodecContext* ctx = nullptr; SwrContext* swr = nullptr; int bin_samples = 1; std::int64_t seen = 0;
        std::vector<double> energy; std::vector<float> peaks; std::vector<int> counts; std::vector<float> mono;
        ~Track() { swr_free(&swr); avcodec_free_context(&ctx); }
    };
    std::vector<std::unique_ptr<Track>> tracks;
    for (unsigned i = 0; i < in.value->nb_streams; ++i) {
        auto* par = in.value->streams[i]->codecpar; if (par->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        auto track = std::make_unique<Track>(); track->index = (int)i;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id); if (!codec) continue;
        track->ctx = avcodec_alloc_context3(codec); if (!track->ctx) throw std::bad_alloc();
        av_check(avcodec_parameters_to_context(track->ctx, par), "Configure audio decoder");
        av_check(avcodec_open2(track->ctx, codec, nullptr), "Open audio decoder");
        AVChannelLayout layout = AV_CHANNEL_LAYOUT_MONO;
        av_check(swr_alloc_set_opts2(&track->swr, &layout, AV_SAMPLE_FMT_FLT, track->ctx->sample_rate, &track->ctx->ch_layout, track->ctx->sample_fmt, track->ctx->sample_rate, 0, nullptr), "Mix audio");
        av_check(swr_init(track->swr), "Mix audio");
        track->bin_samples = std::max(1, track->ctx->sample_rate * bin_ms / 1000);
        tracks.push_back(std::move(track));
    }
    if (tracks.empty()) return {};
    AVPacket* packet = av_packet_alloc(); AVFrame* frame = av_frame_alloc();
    auto consume = [&](Track& t, AVFrame* f) {
        t.mono.resize((size_t)f->nb_samples); std::uint8_t* out = reinterpret_cast<std::uint8_t*>(t.mono.data());
        int got = swr_convert(t.swr, &out, f->nb_samples, (const std::uint8_t**)f->extended_data, f->nb_samples);
        for (int i = 0; i < got; ++i, ++t.seen) {
            size_t bin = (size_t)(t.seen / t.bin_samples);
            if (bin >= t.energy.size()) { t.energy.resize(bin + 1, 0.0); t.peaks.resize(bin + 1, 0.f); t.counts.resize(bin + 1, 0); }
            float v = t.mono[i]; t.energy[bin] += (double)v * v; t.peaks[bin] = std::max(t.peaks[bin], std::abs(v)); ++t.counts[bin];
        }
    };
    auto drain = [&](Track& t) {
        while (true) {
            int r = avcodec_receive_frame(t.ctx, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
            av_check(r, "Decode audio"); consume(t, frame); av_frame_unref(frame);
        }
    };
    try {
        int r;
        while ((r = av_read_frame(in.value, packet)) >= 0) {
            for (auto& t : tracks) if (packet->stream_index == t->index) { avcodec_send_packet(t->ctx, packet); drain(*t); }
            av_packet_unref(packet);
        }
        for (auto& t : tracks) { avcodec_send_packet(t->ctx, nullptr); drain(*t); }
    } catch (...) { av_packet_free(&packet); av_frame_free(&frame); throw; }
    av_packet_free(&packet); av_frame_free(&frame);
    std::vector<AudioLevels> result; auto byte = [](double v) { return (std::uint8_t)std::lround(std::clamp(v, 0.0, 1.0) * 255.0); };
    for (auto& t : tracks) {
        AudioLevels levels; levels.bin_ms = bin_ms; levels.peak.resize(t->energy.size()); levels.rms.resize(t->energy.size());
        for (size_t i = 0; i < t->energy.size(); ++i) { levels.peak[i] = byte(t->peaks[i]); levels.rms[i] = byte(t->counts[i] ? std::sqrt(t->energy[i] / t->counts[i]) : 0.0); }
        result.push_back(std::move(levels));
    }
    return result;
}
std::string encode_levels(const std::vector<std::uint8_t>& levels) {
    static const char digits[] = "0123456789abcdef"; std::string text; text.reserve(levels.size() * 2);
    for (auto v : levels) { text += digits[v >> 4]; text += digits[v & 15]; }
    return text;
}
std::vector<std::uint8_t> decode_levels(const std::string& text) {
    auto nibble = [](char c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; };
    std::vector<std::uint8_t> levels; levels.reserve(text.size() / 2);
    for (size_t i = 0; i + 1 < text.size(); i += 2) levels.push_back((std::uint8_t)((nibble(text[i]) << 4) | nibble(text[i + 1])));
    return levels;
}
AudioLevels downsample_levels(const AudioLevels& levels, int to_bin_ms) {
    if (levels.empty() || levels.bin_ms <= 0 || to_bin_ms <= levels.bin_ms) return levels;
    size_t group = (size_t)std::max(1, to_bin_ms / levels.bin_ms), count = (levels.peak.size() + group - 1) / group;
    AudioLevels result; result.bin_ms = levels.bin_ms * (int)group; result.peak.resize(count); result.rms.resize(count);
    for (size_t i = 0; i < count; ++i) {
        size_t begin = i * group, end = std::min(levels.peak.size(), begin + group); unsigned top = 0; double squares = 0;
        for (size_t k = begin; k < end; ++k) { top = std::max<unsigned>(top, levels.peak[k]); double r = levels.rms[k]; squares += r * r; }
        result.peak[i] = (std::uint8_t)top; result.rms[i] = (std::uint8_t)std::lround(std::sqrt(squares / (end - begin)));
    }
    return result;
}
std::vector<AudioLevels> downsample_levels(const std::vector<AudioLevels>& tracks, int to_bin_ms) {
    std::vector<AudioLevels> result; for (auto& t : tracks) result.push_back(downsample_levels(t, to_bin_ms)); return result;
}
static const char* level_key(size_t track, const char* what, char* buffer) {
    // "audio_peak" for the first track, "audio2_peak" for the second.
    if (track == 0) snprintf(buffer, 32, "audio_%s", what); else snprintf(buffer, 32, "audio%zu_%s", track + 1, what);
    return buffer;
}
std::vector<AudioLevels> read_levels(obs_data_t* d) {
    std::vector<AudioLevels> tracks; char key[32];
    int bin_ms = (int)obs_data_get_int(d, "audio_bin_ms"); if (bin_ms <= 0) bin_ms = AudioBinMs;
    for (size_t track = 0; track < 4; ++track) {
        if (!obs_data_has_user_value(d, level_key(track, "peak", key))) break;
        AudioLevels levels; levels.bin_ms = bin_ms;
        levels.peak = decode_levels(obs_data_get_string(d, level_key(track, "peak", key)));
        levels.rms = decode_levels(obs_data_get_string(d, level_key(track, "rms", key))); levels.rms.resize(levels.peak.size(), 0);
        tracks.push_back(std::move(levels));
    }
    return tracks;
}
void write_levels(obs_data_t* d, const std::vector<AudioLevels>& tracks) {
    char key[32];
    for (size_t track = 0; track < tracks.size(); ++track) {
        obs_data_set_string(d, level_key(track, "peak", key), encode_levels(tracks[track].peak).c_str());
        obs_data_set_string(d, level_key(track, "rms", key), encode_levels(tracks[track].rms).c_str());
    }
    obs_data_set_int(d, "audio_bin_ms", tracks.empty() ? AudioBinMs : tracks.front().bin_ms);
}
void remux(const std::vector<fs::path>& segments, const fs::path& destination) {
    if (segments.empty()) throw std::runtime_error("No completed footage to save");
    fs::create_directories(destination.parent_path());
    fs::path temp = destination; temp += L".partial";
    AVFormatContext* out = nullptr;
    av_check(avformat_alloc_output_context2(&out, nullptr, "mp4", path_text(temp).c_str()), "Create clip");
    AVPacket* packet = av_packet_alloc();
    try {
        Input first(segments.front());
        for (unsigned i = 0; i < first.value->nb_streams; ++i) {
            auto* stream = avformat_new_stream(out, nullptr); if (!stream) throw std::bad_alloc();
            av_check(avcodec_parameters_copy(stream->codecpar, first.value->streams[i]->codecpar), "Copy stream settings");
            stream->codecpar->codec_tag = 0; stream->time_base = first.value->streams[i]->time_base;
        }
        av_check(avio_open(&out->pb, path_text(temp).c_str(), AVIO_FLAG_WRITE), "Open clip output");
        av_check(avformat_write_header(out, nullptr), "Write clip header");
        // Each OBS segment resets each track's timestamps. Continue tracks at
        // their own previous end to avoid accumulating AAC rounding gaps.
        std::vector<int64_t> next(out->nb_streams, 0);
        for (const auto& path : segments) {
            Input in(path);
            if (in.value->nb_streams != out->nb_streams) throw std::runtime_error("Audio/video layout changed between recordings. Save a range within one session.");
            for (unsigned i = 0; i < out->nb_streams; ++i) {
                auto* a = in.value->streams[i]->codecpar; auto* b = out->streams[i]->codecpar;
                if (a->codec_id != b->codec_id || a->width != b->width || a->height != b->height || a->sample_rate != b->sample_rate)
                    throw std::runtime_error("Recording format changed between segments");
            }
            auto offsets = next;
            std::vector<int64_t> first_dts(out->nb_streams, AV_NOPTS_VALUE);
            int read_result;
            while ((read_result = av_read_frame(in.value, packet)) >= 0) {
                auto index = packet->stream_index;
                auto* source = in.value->streams[index]; auto* target = out->streams[index];
                av_packet_rescale_ts(packet, source->time_base, target->time_base);
                if (packet->dts == AV_NOPTS_VALUE || packet->pts == AV_NOPTS_VALUE) { av_packet_unref(packet); continue; }
                if (first_dts[index] == AV_NOPTS_VALUE) first_dts[index] = packet->dts;
                packet->dts += offsets[index] - first_dts[index]; packet->pts += offsets[index] - first_dts[index];
                if (packet->duration <= 0 && target->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                    packet->duration = av_rescale_q(1, AVRational{1, 60}, target->time_base);
                next[index] = std::max(next[index], packet->dts + std::max<int64_t>(1, packet->duration));
                packet->pos = -1;
                av_check(av_interleaved_write_frame(out, packet), "Write clip packet");
            }
            if (read_result != AVERROR_EOF) av_check(read_result, "Read segment");
        }
        av_check(av_write_trailer(out), "Finish clip");
        av_check(avio_closep(&out->pb), "Close clip");
        av_packet_free(&packet); avformat_free_context(out); out = nullptr;
        inspect_media(temp);
        if (!flush_closed(temp)) throw std::runtime_error("Could not flush completed clip to disk");
        if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Could not publish completed clip");
    } catch (...) {
        av_packet_free(&packet);
        if (out) { if (out->pb) avio_closep(&out->pb); avformat_free_context(out); }
        // Retain .partial and protected source segments for recovery.
        throw;
    }
}
}

namespace clip {
HwDevice::~HwDevice() { av_buffer_unref(&ref); }
std::shared_ptr<HwDevice> hw_device(ID3D11Device* render_device) {
    auto result = std::make_shared<HwDevice>();
    if (render_device) {
        result->ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA); if (!result->ref) return {};
        auto* hw = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(result->ref->data)->hwctx);
        render_device->AddRef(); hw->device = render_device;
        if (av_hwdevice_ctx_init(result->ref) < 0) return {};
    } else if (av_hwdevice_ctx_create(&result->ref, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0) return {};
    return result;
}
std::string adapter_name(ID3D11Device* device) {
    IDXGIDevice* dxgi = nullptr; IDXGIAdapter* adapter = nullptr; std::string name = "unknown adapter";
    if (device && SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi))) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) name = utf8(desc.Description);
        if (adapter) adapter->Release(); dxgi->Release();
    }
    return name;
}
std::string adapter_name(const HwDevice& device) {
    if (!device.ref) return "none";
    auto* hw = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device.ref->data)->hwctx);
    return adapter_name(hw->device);
}
AVPixelFormat hw_get_format(AVCodecContext* ctx, const AVPixelFormat* formats) {
    auto* binding = static_cast<HwBinding*>(ctx->opaque);
    for (auto* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f != AV_PIX_FMT_D3D11 || !binding || !binding->device) continue;
        // A pool that already fits this stream is kept: rebuilding it means a
        // fresh video decoder and a new array texture every time.
        if (ctx->hw_frames_ctx) {
            auto* have = reinterpret_cast<AVHWFramesContext*>(ctx->hw_frames_ctx->data);
            if (have->width >= ctx->coded_width && have->height >= ctx->coded_height && have->format == AV_PIX_FMT_D3D11) return AV_PIX_FMT_D3D11;
        }
        AVBufferRef* frames = nullptr;
        if (avcodec_get_hw_frames_parameters(ctx, binding->device->ref, AV_PIX_FMT_D3D11, &frames) < 0) continue;
        auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
        auto* d3d = static_cast<AVD3D11VAFramesContext*>(fc->hwctx);
        d3d->BindFlags = D3D11_BIND_DECODER | (binding->shader_resource ? D3D11_BIND_SHADER_RESOURCE : 0);
        fc->initial_pool_size += binding->extra_frames;
        if (av_hwframe_ctx_init(frames) < 0) { av_buffer_unref(&frames); continue; }
        av_buffer_unref(&ctx->hw_frames_ctx); ctx->hw_frames_ctx = frames; return AV_PIX_FMT_D3D11;
    }
    for (auto* f = formats; *f != AV_PIX_FMT_NONE; ++f) if (!(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f;
    return formats[0];
}
const AVCodec* pick_decoder(int codec_id, bool hardware) {
    if (hardware && codec_id == AV_CODEC_ID_AV1) if (auto* native = avcodec_find_decoder_by_name("av1")) return native;
    return avcodec_find_decoder((AVCodecID)codec_id);
}
struct Decoder::State {
    std::shared_ptr<HwDevice> hw; HwBinding binding; bool hw_failed = false;
    AVCodecContext* ctx = nullptr; AVCodecID codec_id = AV_CODEC_ID_NONE; int width = 0, height = 0; std::vector<std::uint8_t> extradata;
    AVPacket* packet = av_packet_alloc(); AVFrame* frame = av_frame_alloc(); AVFrame* best = av_frame_alloc(); AVFrame* cpu = av_frame_alloc();
    SwsContext* sws = nullptr; int sws_w = 0, sws_h = 0, sws_src_w = 0, sws_src_h = 0, sws_fmt = -1;
    ~State() { avcodec_free_context(&ctx); av_packet_free(&packet); av_frame_free(&frame); av_frame_free(&best); av_frame_free(&cpu); sws_freeContext(sws); }
    // Reuse the context when the stream matches the previous one.
    void prepare(AVStream* stream) {
        auto* par = stream->codecpar;
        bool same = ctx && par->codec_id == codec_id && par->width == width && par->height == height &&
            extradata.size() == (size_t)par->extradata_size && (par->extradata_size == 0 || memcmp(extradata.data(), par->extradata, par->extradata_size) == 0);
        if (same) { avcodec_flush_buffers(ctx); return; }
        avcodec_free_context(&ctx);
        bool hardware = hw && !hw_failed; const AVCodec* codec = pick_decoder(par->codec_id, hardware);
        if (!codec) throw std::runtime_error("No decoder for the recording's video codec");
        ctx = avcodec_alloc_context3(codec); if (!ctx) throw std::bad_alloc();
        av_check(avcodec_parameters_to_context(ctx, par), "Configure decoder");
        // Hardware decoding runs asynchronously on the GPU; frame threads only add per-thread setup.
        ctx->thread_count = hardware ? 1 : 0;
        if (hardware) { binding = {hw, false, 16}; ctx->opaque = &binding; ctx->get_format = hw_get_format; }
        av_check(avcodec_open2(ctx, codec, nullptr), "Open decoder");
        codec_id = par->codec_id; width = par->width; height = par->height;
        extradata.assign(par->extradata, par->extradata + par->extradata_size);
    }
};
Decoder::Decoder(std::shared_ptr<HwDevice> hw) : s_(new State) { s_->hw = std::move(hw); }
Decoder::~Decoder() { delete s_; }
struct HwFailure {};
Frame decode_with(Decoder::State* s, const fs::path& p, std::int64_t offset_ms, int width, bool keyframe_only, const std::function<bool()>& cancelled);
Frame Decoder::decode(const fs::path& p, std::int64_t offset_ms, int width, bool keyframe_only, const std::function<bool()>& cancelled) {
    try { return decode_with(s_, p, offset_ms, width, keyframe_only, cancelled); }
    catch (const HwFailure&) {
        // The native decoder could not use the device: fall back to software for good.
        if (cancelled && cancelled()) throw std::runtime_error("Decode cancelled");
        s_->hw_failed = true; avcodec_free_context(&s_->ctx);
        return decode_with(s_, p, offset_ms, width, keyframe_only, cancelled);
    }
}
Frame decode_with(Decoder::State* s_, const fs::path& p, std::int64_t offset_ms, int width, bool keyframe_only, const std::function<bool()>& cancelled) {
    // Every exit releases per-request packet/frame references, including cancellation
    // during hardware fallback, transfer failure, and a thrown scaling error.
    struct ReleaseFrames {
        Decoder::State* state;
        ~ReleaseFrames() { av_packet_unref(state->packet); av_frame_unref(state->frame); av_frame_unref(state->best); av_frame_unref(state->cpu); }
    } release{s_};
    auto check_cancel = [&] { if (cancelled && cancelled()) throw std::runtime_error("Decode cancelled"); };
    check_cancel();
    Input in(p, false); int index = -1;
    for (unsigned i = 0; i < in.value->nb_streams; ++i) if (in.value->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { index = (int)i; break; }
    if (index < 0) throw std::runtime_error("No video stream: " + path_text(p));
    auto* stream = in.value->streams[index];
    s_->prepare(stream); auto* ctx = s_->ctx;
    ctx->skip_frame = keyframe_only ? AVDISCARD_NONKEY : AVDISCARD_DEFAULT;
    std::int64_t target = av_rescale_q(offset_ms, AVRational{1, 1000}, stream->time_base);
    if (offset_ms > 0) av_seek_frame(in.value, index, target, AVSEEK_FLAG_BACKWARD);
    bool have = false, done = false; AVFrame* frame = s_->frame; AVFrame* best = s_->best; AVPacket* packet = s_->packet;
    av_frame_unref(best);
    auto consider = [&](AVFrame* f) {
        std::int64_t pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
        if (pts != AV_NOPTS_VALUE && pts > target && have) { done = true; return; }
        av_frame_unref(best); av_frame_ref(best, f); have = true;
        if (keyframe_only || (pts != AV_NOPTS_VALUE && pts >= target)) done = true;
    };
    bool hardware = s_->hw && !s_->hw_failed;
    while (!done) {
        check_cancel();
        int r = av_read_frame(in.value, packet);
        int sent = 0;
        if (r < 0) { avcodec_send_packet(ctx, nullptr); }
        else if (packet->stream_index != index) { av_packet_unref(packet); continue; }
        else { sent = avcodec_send_packet(ctx, packet); av_packet_unref(packet); }
        if (sent < 0 && sent != AVERROR(EAGAIN) && hardware && !have) throw HwFailure{};
        while (!done) {
            check_cancel();
            int rr = avcodec_receive_frame(ctx, frame);
            if (rr == AVERROR(EAGAIN)) break;
            if (rr == AVERROR_EOF) { done = true; break; }
            if (rr < 0) { if (hardware && !have) throw HwFailure{}; done = true; break; }
            consider(frame); av_frame_unref(frame);
        }
        if (r < 0) break;
    }
    if (!have) throw std::runtime_error("No decodable frame at that position");
    check_cancel();
    AVFrame* picture = best;
    if (best->format == AV_PIX_FMT_D3D11) { av_frame_unref(s_->cpu); av_check(av_hwframe_transfer_data(s_->cpu, best, 0), "Download decoded frame"); picture = s_->cpu; }
    int height = std::max(1, (int)std::lround((double)picture->height * width / std::max(1, picture->width)));
    if (!s_->sws || s_->sws_w != width || s_->sws_h != height || s_->sws_src_w != picture->width || s_->sws_src_h != picture->height || s_->sws_fmt != picture->format) {
        sws_freeContext(s_->sws);
        s_->sws = sws_getContext(picture->width, picture->height, (AVPixelFormat)picture->format, width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        s_->sws_w = width; s_->sws_h = height; s_->sws_src_w = picture->width; s_->sws_src_h = picture->height; s_->sws_fmt = picture->format;
    }
    if (!s_->sws) throw std::runtime_error("Cannot scale decoded frame");
    Frame out; out.width = width; out.height = height; out.rgba.resize((size_t)width * height * 4);
    std::uint8_t* planes[1] = {out.rgba.data()}; int strides[1] = {width * 4};
    sws_scale(s_->sws, picture->data, picture->linesize, 0, picture->height, planes, strides);
    std::int64_t pts = best->best_effort_timestamp == AV_NOPTS_VALUE ? best->pts : best->best_effort_timestamp;
    out.pts_ms = pts == AV_NOPTS_VALUE ? offset_ms : av_rescale_q(pts, stream->time_base, AVRational{1, 1000});
    av_frame_unref(best); av_frame_unref(s_->cpu); // Release the hardware surface before the next call.
    return out;
}
Frame decode_frame(const fs::path& p, std::int64_t offset_ms, int width, bool keyframe_only, std::shared_ptr<HwDevice> hw) {
    Decoder decoder(std::move(hw)); return decoder.decode(p, offset_ms, width, keyframe_only);
}
}
