#include "export.hpp"
#include "media.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace clip {
namespace {
constexpr std::int64_t Million = 1000000;
// Pinned DXVA reserves at most 17 reference/work surfaces. These 32 extra
// slots keep the single decoder array below its 64-slice driver limit while
// covering NVENC's 16-frame output delay plus reordering and the current frame.
constexpr int GpuInputExtraFrames = 32;
constexpr int GpuInputDelay = 16;
template <class T, void (*Free)(T**)> struct Owned {
    T* value = nullptr;
    Owned() = default; explicit Owned(T* v) : value(v) {}
    Owned(const Owned&) = delete; Owned& operator=(const Owned&) = delete;
    Owned(Owned&& other) noexcept : value(other.value) { other.value = nullptr; }
    Owned& operator=(Owned&& other) noexcept { if (this != &other) { Free(&value); value = other.value; other.value = nullptr; } return *this; }
    ~Owned() { Free(&value); }
    T* operator->() const { return value; } operator T*() const { return value; }
    void reset(T* v = nullptr) { Free(&value); value = v; }
};
void free_fifo(AVAudioFifo** f) { if (*f) { av_audio_fifo_free(*f); *f = nullptr; } }
void free_scaler(SwsContext** s) { sws_freeContext(*s); *s = nullptr; }
using CodecContext = Owned<AVCodecContext, avcodec_free_context>;
using Frame = Owned<AVFrame, av_frame_free>;
using Packet = Owned<AVPacket, av_packet_free>;
using Fifo = Owned<AVAudioFifo, free_fifo>;
using Resampler = Owned<SwrContext, swr_free>;
using Scaler = Owned<SwsContext, free_scaler>;
using Format = Owned<AVFormatContext, avformat_close_input>;
struct HwInputFailure {};
struct HwFailure {};

bool supports(const AVCodecContext* ctx, const AVCodec* codec, AVPixelFormat wanted) {
    const AVPixelFormat* formats = nullptr; int count = 0;
    if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void**)&formats, &count) < 0 || !formats) return true;
    for (int i = 0; i < count; ++i) if (formats[i] == wanted) return true;
    return false;
}
// Open the first usable video encoder for the request. NVENC first; H.264
// additionally has a software fallback so an export never depends on the GPU.
AVCodecContext* open_video_encoder(const ExportRequest& r, int src_w, int src_h, std::string& name_out, AVFrame* input) {
    int height = r.height & ~1, width = ((int)std::lround((double)src_w * height / std::max(1, src_h)) + 1) & ~1;
    std::vector<const char*> names = r.codec == "av1" ? std::vector<const char*>{"av1_nvenc"} : std::vector<const char*>{"h264_nvenc", "libx264"};
    std::string errors;
    for (auto name : names) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) { errors += std::string(name) + " missing; "; continue; }
        auto* pool = input && input->hw_frames_ctx ? reinterpret_cast<AVHWFramesContext*>(input->hw_frames_ctx->data) : nullptr;
        bool gpu = std::strstr(name, "nvenc") && input && input->format == AV_PIX_FMT_D3D11 &&
            input->width == width && input->height == height && pool && pool->format == AV_PIX_FMT_D3D11 && pool->sw_format == AV_PIX_FMT_NV12;
        for (int attempt = gpu ? 0 : 1; attempt < 2; ++attempt) {
            CodecContext ctx(avcodec_alloc_context3(codec)); if (!ctx) throw std::bad_alloc();
            ctx->width = width; ctx->height = height; ctx->time_base = {1, r.fps}; ctx->framerate = {r.fps, 1};
            ctx->pix_fmt = supports(ctx, codec, AV_PIX_FMT_NV12) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
            if (attempt == 0) {
                ctx->pix_fmt = AV_PIX_FMT_D3D11;
                ctx->hw_frames_ctx = av_buffer_ref(input->hw_frames_ctx);
                ctx->hw_device_ctx = av_buffer_ref(pool->device_ref);
                if (!ctx->hw_frames_ctx || !ctx->hw_device_ctx) throw std::bad_alloc();
            }
            ctx->bit_rate = (std::int64_t)r.bitrate_kbps * 1000; ctx->rc_max_rate = ctx->bit_rate * 3 / 2; ctx->rc_buffer_size = (int)std::min<std::int64_t>(ctx->bit_rate * 2, INT32_MAX);
            ctx->gop_size = r.fps * 2; ctx->color_range = AVCOL_RANGE_MPEG; ctx->colorspace = AVCOL_SPC_BT709;
            ctx->color_primaries = AVCOL_PRI_BT709; ctx->color_trc = AVCOL_TRC_BT709; ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            AVDictionary* options = nullptr;
            if (std::strstr(name, "nvenc")) { av_dict_set(&options, "preset", "p5", 0); av_dict_set(&options, "rc", "vbr", 0); }
            else { av_dict_set(&options, "preset", "fast", 0); ctx->thread_count = 0; }
            if (attempt == 0) av_dict_set_int(&options, "delay", GpuInputDelay, 0);
            if (r.codec != "av1") av_dict_set(&options, "profile", "high", 0);
            int result = avcodec_open2(ctx, codec, &options); av_dict_free(&options);
            if (result < 0) { char text[AV_ERROR_MAX_STRING_SIZE]; av_strerror(result, text, sizeof(text)); errors += std::string(name) + ": " + text + "; "; continue; }
            name_out = name; auto* raw = ctx.value; ctx.value = nullptr; return raw;
        }
    }
    throw std::runtime_error("No usable video encoder (" + errors + "). Check the NVIDIA driver.");
}
AVCodecContext* open_audio_encoder(const AVCodecContext* source) {
    const AVCodec* codec = avcodec_find_encoder_by_name("aac"); if (!codec) return nullptr;
    CodecContext ctx(avcodec_alloc_context3(codec)); if (!ctx) throw std::bad_alloc();
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP; ctx->sample_rate = source->sample_rate; ctx->bit_rate = 192000;
    av_channel_layout_copy(&ctx->ch_layout, &source->ch_layout);
    if (ctx->ch_layout.nb_channels > 2) { av_channel_layout_uninit(&ctx->ch_layout); av_channel_layout_default(&ctx->ch_layout, 2); }
    ctx->time_base = {1, ctx->sample_rate}; ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(ctx, codec, nullptr) < 0) return nullptr;
    auto* raw = ctx.value; ctx.value = nullptr; return raw;
}
// One requested audio track's decoder within the open segment.
struct AudioDecoder { int index = -1; CodecContext ctx; Resampler swr; std::int64_t base_us = -1, samples = 0; };
// One open segment: demuxer, decoders, and their conversion state.
struct Source {
    Format fmt; Span span; int vindex = -1;
    CodecContext video; HwBinding binding; bool hardware = false;
    std::vector<AudioDecoder> audio; // One per requested track; index -1 when the segment lacks that track.
};
struct Exporter {
    const ExportRequest& r; std::shared_ptr<HwDevice> hw; std::atomic<double>* progress;
    std::int64_t in_us, out_us, total_frames, total_samples = 0;
    AVFormatContext* out = nullptr; AVStream* vstream = nullptr; AVStream* astream = nullptr;
    bool force_cpu_input = false;
    CodecContext venc, aenc; Packet packet{av_packet_alloc()}; Frame decoded{av_frame_alloc()}, cpu{av_frame_alloc()}, scaled{av_frame_alloc()}, audio_frame{av_frame_alloc()};
    Scaler sws; int sws_src_w = 0, sws_src_h = 0, sws_fmt = -1;
    // Requested audio tracks by stream ordinal (0 desktop, 1 microphone). Each
    // has a queue of converted samples placed on the output grid; the mix
    // consumes them in step, padding a track that has fallen behind or is
    // absent from the current segment with silence.
    struct Track { int ordinal; float gain; Fifo fifo; std::int64_t written = 0; };
    std::vector<Track> tracks; std::int64_t frames_out = 0, mixed_out = 0, audio_pts = 0; bool video_done = false, audio_done = false;
    std::string decoder_name, encoder_name; fs::path temp_path;
    Exporter(const ExportRequest& request, std::atomic<double>* p) : r(request), progress(p) {
        in_us = request.start_ms * 1000; out_us = request.end_ms * 1000;
        total_frames = std::max<std::int64_t>(1, (out_us - in_us) * request.fps / Million);
        if (request.audio) tracks.push_back({0, (float)request.desktop_gain}); if (request.mic) tracks.push_back({1, (float)request.mic_gain});
    }
    ~Exporter() { if (out) { if (out->pb) avio_closep(&out->pb); avformat_free_context(out); } }
    std::int64_t due_us(std::int64_t n) const { return in_us + n * Million / r.fps; }
    void write(AVCodecContext* enc, AVStream* stream) {
        while (true) {
            int result = avcodec_receive_packet(enc, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            av_check(result, "Encode");
            av_packet_rescale_ts(packet, enc->time_base, stream->time_base); packet->stream_index = stream->index;
            int written = av_interleaved_write_frame(out, packet); av_packet_unref(packet); av_check(written, "Write clip packet");
        }
    }
    void encode_video(AVFrame* frame) {
        AVFrame* picture = frame;
        if (venc->pix_fmt == AV_PIX_FMT_D3D11) {
            auto* pool = frame->hw_frames_ctx ? reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data) : nullptr;
            auto* initial = reinterpret_cast<AVHWFramesContext*>(venc->hw_frames_ctx->data);
            if (frame->format != AV_PIX_FMT_D3D11 || frame->width != venc->width || frame->height != venc->height ||
                !pool || pool != initial || pool->sw_format != AV_PIX_FMT_NV12) throw HwInputFailure{};
        } else {
            // Scaling and software encoders keep the existing CPU input path.
            if (frame->format == AV_PIX_FMT_D3D11) { av_frame_unref(cpu); av_check(av_hwframe_transfer_data(cpu, frame, 0), "Download decoded frame"); picture = cpu; }
            if (picture->width != venc->width || picture->height != venc->height || picture->format != venc->pix_fmt) {
                if (!sws || sws_src_w != picture->width || sws_src_h != picture->height || sws_fmt != picture->format) {
                    sws.reset(sws_getContext(picture->width, picture->height, (AVPixelFormat)picture->format, venc->width, venc->height, venc->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr));
                    if (!sws) throw std::runtime_error("Cannot scale decoded frame");
                    sws_src_w = picture->width; sws_src_h = picture->height; sws_fmt = picture->format;
                }
                if (!scaled->data[0]) { scaled->width = venc->width; scaled->height = venc->height; scaled->format = venc->pix_fmt; av_check(av_frame_get_buffer(scaled, 32), "Allocate frame"); }
                av_check(av_frame_make_writable(scaled), "Prepare frame");
                sws_scale(sws, picture->data, picture->linesize, 0, picture->height, scaled->data, scaled->linesize);
                picture = scaled;
            }
        }
        picture->pts = frames_out; picture->color_range = AVCOL_RANGE_MPEG;
        int sent = avcodec_send_frame(venc, picture);
        if (sent < 0 && venc->pix_fmt == AV_PIX_FMT_D3D11) throw HwInputFailure{};
        av_check(sent, "Encode video"); write(venc, vstream);
        if (progress) *progress = std::clamp((double)++frames_out / (double)total_frames, 0.0, 1.0);
        else ++frames_out;
        if (frames_out >= total_frames) video_done = true;
    }
    // Mix the tracks' queues in step and encode. Without `flush`, a frame is
    // taken only when every track present in the current segment has one
    // ready; absent or exhausted tracks contribute silence.
    void mix_encode(const Source& src, bool flush) {
        if (!aenc) return;
        int size = aenc->frame_size > 0 ? aenc->frame_size : 1024, channels = aenc->ch_layout.nb_channels;
        while (true) {
            int have = INT_MAX, most = 0; bool any_present = false;
            for (size_t k = 0; k < tracks.size(); ++k) {
                int n = av_audio_fifo_size(tracks[k].fifo); most = std::max(most, n);
                if (k < src.audio.size() && src.audio[k].index >= 0) { any_present = true; have = std::min(have, n); }
            }
            int n = flush ? std::min(size, most) : size;
            if (!flush && (!any_present || have < size)) return;
            n = (int)std::min<std::int64_t>(n, total_samples - mixed_out); // Never past the out point.
            if (n <= 0) return;
            av_frame_unref(audio_frame); audio_frame->nb_samples = n; audio_frame->format = aenc->sample_fmt; audio_frame->sample_rate = aenc->sample_rate;
            av_channel_layout_copy(&audio_frame->ch_layout, &aenc->ch_layout);
            av_check(av_frame_get_buffer(audio_frame, 0), "Allocate audio frame");
            for (int c = 0; c < channels; ++c) std::memset(audio_frame->data[c], 0, (size_t)n * sizeof(float));
            std::uint8_t* planes[AV_NUM_DATA_POINTERS] = {};
            av_check(av_samples_alloc(planes, nullptr, channels, n, aenc->sample_fmt, 0), "Mix audio");
            for (auto& track : tracks) {
                int got = std::max(0, av_audio_fifo_read(track.fifo, (void**)planes, n));
                for (int c = 0; c < channels; ++c) {
                    auto* mix = reinterpret_cast<float*>(audio_frame->data[c]); auto* in = reinterpret_cast<const float*>(planes[c]);
                    for (int i = 0; i < got; ++i) mix[i] += in[i] * track.gain;
                }
                track.written = std::max(track.written, mixed_out + n);
            }
            av_freep(&planes[0]);
            for (int c = 0; c < channels; ++c) { auto* mix = reinterpret_cast<float*>(audio_frame->data[c]); for (int i = 0; i < n; ++i) mix[i] = std::clamp(mix[i], -1.f, 1.f); }
            audio_frame->pts = audio_pts; audio_pts += n; mixed_out += n;
            av_check(avcodec_send_frame(aenc, audio_frame), "Encode audio"); write(aenc, astream);
            if (mixed_out >= total_samples) { audio_done = true; return; }
        }
    }
    // Place decoded samples of one track on the output sample grid: skip what
    // is already written, fill any gap with silence, stop at the out point.
    void place_audio(Source& src, size_t k, AVFrame* frame) {
        auto& dec = src.audio[k]; auto& track = tracks[k];
        if (dec.base_us < 0) {
            std::int64_t pts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) return;
            dec.base_us = src.span.start_ms * 1000 + av_rescale_q(pts, src.fmt->streams[dec.index]->time_base, AVRational{1, Million});
            dec.samples = 0;
        }
        int rate = aenc->sample_rate, channels = aenc->ch_layout.nb_channels;
        std::int64_t first = av_rescale(dec.base_us - in_us, rate, Million) + dec.samples; // Output index of this frame's first sample.
        dec.samples += frame->nb_samples;
        std::uint8_t* converted[AV_NUM_DATA_POINTERS] = {};
        int count = swr_get_out_samples(dec.swr, frame->nb_samples); if (count <= 0) return;
        av_check(av_samples_alloc(converted, nullptr, channels, count, aenc->sample_fmt, 0), "Convert audio");
        count = swr_convert(dec.swr, converted, count, (const std::uint8_t**)frame->extended_data, frame->nb_samples);
        if (count > 0) {
            std::int64_t skip = std::clamp<std::int64_t>(track.written - first, 0, count);
            std::int64_t gap = std::clamp<std::int64_t>(first - track.written, 0, std::min<std::int64_t>(rate, total_samples - track.written)); // Bounded silence for a seam or dropout.
            if (gap > 0) {
                std::uint8_t* silence[AV_NUM_DATA_POINTERS] = {};
                av_check(av_samples_alloc(silence, nullptr, channels, (int)gap, aenc->sample_fmt, 0), "Fill audio gap");
                av_samples_set_silence(silence, 0, (int)gap, channels, aenc->sample_fmt);
                av_audio_fifo_write(track.fifo, (void**)silence, (int)gap); track.written += gap; av_freep(&silence[0]);
            }
            std::int64_t take = std::min<std::int64_t>(count - skip, total_samples - track.written);
            if (take > 0) {
                std::uint8_t* planes[AV_NUM_DATA_POINTERS] = {}; int bytes = av_get_bytes_per_sample(aenc->sample_fmt);
                for (int c = 0; c < channels; ++c) planes[c] = converted[c] + skip * bytes;
                av_audio_fifo_write(track.fifo, (void**)planes, (int)take); track.written += take;
            }
        }
        av_freep(&converted[0]);
        mix_encode(src, false);
    }
    void open(Source& src, const Span& span, bool hardware) {
        src.fmt.reset(); src.audio.clear(); src.vindex = -1;
        AVFormatContext* fmt = nullptr;
        if (!open_without_probe(&fmt, span.path)) throw std::runtime_error("Cannot open " + path_text(span.path));
        src.fmt.reset(fmt); src.span = span;
        std::vector<int> audio_streams;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            auto* par = fmt->streams[i]->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && src.vindex < 0) src.vindex = (int)i;
            if (par->codec_type == AVMEDIA_TYPE_AUDIO) audio_streams.push_back((int)i);
        }
        if (src.vindex < 0) throw std::runtime_error("No video stream: " + path_text(span.path));
        auto* vpar = fmt->streams[src.vindex]->codecpar;
        bool use_hw = hardware && hw != nullptr;
        bool reuse = src.video && src.hardware == use_hw && src.video->codec_id == vpar->codec_id &&
            src.video->width == vpar->width && src.video->height == vpar->height &&
            src.video->extradata_size == vpar->extradata_size &&
            (!vpar->extradata_size || std::memcmp(src.video->extradata, vpar->extradata, vpar->extradata_size) == 0);
        if (!reuse && venc && venc->pix_fmt == AV_PIX_FMT_D3D11) throw HwInputFailure{};
        src.hardware = use_hw;
        const AVCodec* vcodec = pick_decoder(vpar->codec_id, src.hardware);
        if (!vcodec) throw std::runtime_error("No decoder for the recording's video codec");
        // Matching segments reuse the decoder and its texture pool. NVENC's
        // pool reference stays valid without retaining one pool per segment.
        if (reuse) avcodec_flush_buffers(src.video);
        else {
            src.video.reset(avcodec_alloc_context3(vcodec)); av_check(avcodec_parameters_to_context(src.video, vpar), "Configure decoder");
            src.video->thread_count = src.hardware ? 1 : 0;
            if (src.hardware) {
                bool same_size = (r.height & ~1) == vpar->height && !(vpar->width & 1);
                src.binding = {hw, false, !force_cpu_input && same_size ? GpuInputExtraFrames : 8};
                src.video->opaque = &src.binding; src.video->get_format = hw_get_format;
            }
            av_check(avcodec_open2(src.video, vcodec, nullptr), "Open decoder");
        }
        decoder_name = std::string(vcodec->name) + (src.hardware ? " (D3D11VA)" : " (software)");
        src.audio.resize(tracks.size());
        for (size_t k = 0; k < tracks.size(); ++k) {
            auto& dec = src.audio[k]; size_t ordinal = (size_t)tracks[k].ordinal;
            if (ordinal >= audio_streams.size()) continue; // This segment has no such track: silence.
            auto* apar = fmt->streams[audio_streams[ordinal]]->codecpar; const AVCodec* acodec = avcodec_find_decoder(apar->codec_id);
            if (!acodec) continue;
            dec.ctx.reset(avcodec_alloc_context3(acodec));
            if (avcodec_parameters_to_context(dec.ctx, apar) < 0 || avcodec_open2(dec.ctx, acodec, nullptr) < 0) { dec.ctx.reset(); continue; }
            if (!aenc) { aenc.reset(open_audio_encoder(dec.ctx)); if (!aenc) { dec.ctx.reset(); continue; } }
            SwrContext* swr = nullptr;
            if (swr_alloc_set_opts2(&swr, &aenc->ch_layout, aenc->sample_fmt, aenc->sample_rate, &dec.ctx->ch_layout, dec.ctx->sample_fmt, dec.ctx->sample_rate, 0, nullptr) < 0 || swr_init(swr) < 0) { swr_free(&swr); dec.ctx.reset(); continue; }
            dec.swr.reset(swr); dec.index = audio_streams[ordinal];
        }
    }
    void begin_output(AVFrame* frame) {
        av_check(avformat_alloc_output_context2(&out, nullptr, "mp4", path_text(temp_path).c_str()), "Create clip");
        venc.reset(open_video_encoder(r, frame->width, frame->height, encoder_name, force_cpu_input ? nullptr : frame));
        vstream = avformat_new_stream(out, nullptr); if (!vstream) throw std::bad_alloc();
        av_check(avcodec_parameters_from_context(vstream->codecpar, venc), "Describe video"); vstream->time_base = venc->time_base; vstream->avg_frame_rate = venc->framerate;
        if (aenc) {
            astream = avformat_new_stream(out, nullptr); if (!astream) throw std::bad_alloc();
            av_check(avcodec_parameters_from_context(astream->codecpar, aenc), "Describe audio"); astream->time_base = aenc->time_base;
            for (auto& track : tracks) track.fifo.reset(av_audio_fifo_alloc(aenc->sample_fmt, aenc->ch_layout.nb_channels, aenc->frame_size > 0 ? aenc->frame_size * 4 : 4096));
            total_samples = av_rescale(out_us - in_us, aenc->sample_rate, Million);
        } else audio_done = true;
        av_check(avio_open(&out->pb, path_text(temp_path).c_str(), AVIO_FLAG_WRITE), "Open clip output");
        AVDictionary* options = nullptr; av_dict_set(&options, "movflags", "+faststart", 0);
        int result = avformat_write_header(out, &options); av_dict_free(&options); av_check(result, "Write clip header");
    }
    // Decode one source from its first packet, feeding frames by timeline time.
    // Returns false when the whole export is complete.
    bool run_source(Source& src) {
        auto* vstream_in = src.fmt->streams[src.vindex]; bool frames_seen = false;
        auto video_frame = [&](AVFrame* f) {
            frames_seen = true;
            std::int64_t pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) return;
            std::int64_t t = src.span.start_ms * 1000 + av_rescale_q(pts, vstream_in->time_base, AVRational{1, Million});
            if (!out) begin_output(f);
            // Emit this picture for every output slot it covers; a source
            // frame ahead of the next slot is dropped (60 to 30 fps).
            while (!video_done && due_us(frames_out) <= t + 1000) encode_video(f);
        };
        auto check_decode = [&](int result, const char* message) {
            // Pool exhaustion can occur after successful frames when NVENC is
            // holding decoder slices. Retry once through CPU input; corrupted
            // packets and other decode errors keep their normal failure path.
            if (result == AVERROR(ENOMEM) && venc && venc->pix_fmt == AV_PIX_FMT_D3D11) throw HwInputFailure{};
            if (src.hardware && !frames_seen) throw HwFailure{};
            av_check(result, message);
        };
        auto drain_video = [&] {
            while (true) {
                int result = avcodec_receive_frame(src.video, decoded);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
                if (result < 0) check_decode(result, "Decode");
                video_frame(decoded); av_frame_unref(decoded);
            }
        };
        auto drain_audio = [&](size_t k) {
            while (true) {
                int result = avcodec_receive_frame(src.audio[k].ctx, decoded);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
                if (result < 0) return;
                if (!audio_done && out) place_audio(src, k, decoded);
                av_frame_unref(decoded);
            }
        };
        bool eof = false;
        while (!(video_done && audio_done)) {
            int result = eof ? AVERROR_EOF : av_read_frame(src.fmt, packet);
            if (result < 0) {
                if (result != AVERROR_EOF) av_check(result, "Read segment");
                if (eof) break;
                eof = true;
                int sent = avcodec_send_packet(src.video, nullptr);
                if (sent < 0 && sent != AVERROR(EAGAIN) && sent != AVERROR_EOF) check_decode(sent, "Drain decoder");
                drain_video();
                for (size_t k = 0; k < src.audio.size(); ++k) if (src.audio[k].ctx) { avcodec_send_packet(src.audio[k].ctx, nullptr); drain_audio(k); }
                break;
            }
            if (packet->stream_index == src.vindex) {
                int sent = avcodec_send_packet(src.video, packet); av_packet_unref(packet);
                if (sent < 0 && sent != AVERROR(EAGAIN)) check_decode(sent, "Decode video");
                drain_video();
            } else {
                bool used = false;
                for (size_t k = 0; k < src.audio.size(); ++k) if (src.audio[k].ctx && packet->stream_index == src.audio[k].index) { avcodec_send_packet(src.audio[k].ctx, packet); used = true; av_packet_unref(packet); drain_audio(k); break; }
                if (!used) av_packet_unref(packet);
            }
        }
        return !(video_done && audio_done);
    }
    void finish(const Source& src) {
        if (!out) throw std::runtime_error("No frames in the marked range");
        av_check(avcodec_send_frame(venc, nullptr), "Flush video"); write(venc, vstream);
        if (aenc) { mix_encode(src, true); av_check(avcodec_send_frame(aenc, nullptr), "Flush audio"); write(aenc, astream); }
        av_check(av_write_trailer(out), "Finish clip"); av_check(avio_closep(&out->pb), "Close clip");
    }
};
}

static ExportResult export_clip_impl(const std::vector<Span>& sources, const ExportRequest& request, const fs::path& output, std::atomic<double>* progress, bool force_cpu_input) {
    if (sources.empty()) throw std::runtime_error("No footage in the marked range");
    if (request.end_ms - request.start_ms < 100) throw std::runtime_error("Mark a range of at least 0.1 s.");
    if (request.fps <= 0 || request.height <= 0 || request.bitrate_kbps <= 0) throw std::runtime_error("Invalid export settings");
    fs::create_directories(output.parent_path());
    fs::path temp = output; temp += L".partial";
    auto hw = hw_device(nullptr);
    ExportResult result;
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool hardware = attempt < 2;
        if (hardware && !hw) continue;
        std::error_code ec; fs::remove(temp, ec);
        Exporter exporter(request, progress); exporter.hw = hw; exporter.temp_path = temp; exporter.force_cpu_input = force_cpu_input || attempt > 0;
        try {
            Source src;
            for (auto& span : sources) {
                if (span.end_ms * 1000 <= exporter.in_us) continue; // Entirely before the in point.
                exporter.open(src, span, hardware);
                if (!exporter.run_source(src)) break;
            }
            exporter.finish(src);
            result.video_encoder = exporter.encoder_name; result.decoder = exporter.decoder_name; result.frames = exporter.frames_out; result.gpu_input = exporter.venc->pix_fmt == AV_PIX_FMT_D3D11;
            break;
        } catch (const HwInputFailure&) {
            if (attempt != 0 || force_cpu_input) throw std::runtime_error("Cannot submit hardware frames to the encoder");
            continue; // Redo the whole temporary output using CPU encoder input.
        } catch (const HwFailure&) {
            // The device cannot decode this stream: redo the export in software.
            if (!hardware) throw std::runtime_error("Cannot decode the recording");
            attempt = 1; continue;
        }
    }
    inspect_media(temp);
    if (!flush_closed(temp)) throw std::runtime_error("Could not flush exported clip to disk");
    if (!MoveFileExW(temp.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING)) throw std::runtime_error("Could not publish exported clip");
    if (progress) *progress = 1.0;
    return result;
}

ExportResult export_clip(const std::vector<Span>& sources, const ExportRequest& request, const fs::path& output, std::atomic<double>* progress) {
    return export_clip_impl(sources, request, output, progress, false);
}

int export_test(const fs::path& buffer_root, int seconds) {
    std::string report; int failures = 0;
    auto map = scan_buffer(buffer_root);
    report += "segments: " + std::to_string(map.spans.size()) + "\n";
    if (map.spans.empty()) { atomic_write(app_dir() / "export-test.txt", report + "No closed segments\n"); return 1; }
    // Prefer a seam, but a single short segment is also a useful input.
    size_t seam = map.spans.size() - 1;
    for (size_t i = map.spans.size(); i > 1; --i)
        if (map.spans[i - 1].start_ms == map.spans[i - 2].end_ms && map.spans[i - 1].session == map.spans[i - 2].session) { seam = i - 1; break; }
    bool pair = seam > 0 && map.spans[seam].start_ms == map.spans[seam - 1].end_ms && map.spans[seam].session == map.spans[seam - 1].session;
    ExportRequest request;
    request.start_ms = pair ? std::max(map.spans[seam - 1].start_ms, map.spans[seam].start_ms - 1500) : map.spans[seam].start_ms;
    auto available_end = map.spans[seam].end_ms;
    for (size_t i = seam + 1; i < map.spans.size() && map.spans[i].start_ms == available_end &&
        map.spans[i].session == map.spans[seam].session; ++i) available_end = map.spans[i].end_ms;
    request.end_ms = std::min(available_end, request.start_ms + std::max<std::int64_t>(100, (std::int64_t)seconds * 1000));
    if (request.end_ms - request.start_ms < 100) { atomic_write(app_dir() / "export-test.txt", report + "Less than 100 ms available\n"); return 1; }
    auto sources = spans_in_range(map.spans, request.start_ms, request.end_ms);
    if (sources.empty()) { atomic_write(app_dir() / "export-test.txt", report + "No contiguous footage in test range\n"); return 1; }
    MediaInfo source_info;
    try { source_info = inspect_media(sources.front().path); }
    catch (const std::exception& e) { atomic_write(app_dir() / "export-test.txt", report + e.what() + "\n"); return 1; }
    request.bitrate_kbps = 8000; request.mic = map.spans[seam].coarse.size() > 1;
    double duration = (request.end_ms - request.start_ms) / 1000.0;
    auto run = [&](const char* codec, int fps, int height, bool force_cpu, const std::string& label) {
        request.codec = codec; request.fps = fps; request.height = height;
        auto output = app_dir() / ("export-test-" + label + ".mp4");
        auto began = steady_ms();
        try {
            auto result = export_clip_impl(sources, request, output, nullptr, force_cpu);
            auto info = inspect_media(output); auto video = probe_video(output);
            std::int64_t expected = (request.end_ms - request.start_ms) * fps / 1000;
            int expected_height = height & ~1;
            int expected_width = ((int)std::lround((double)source_info.width * expected_height / std::max(1, source_info.height)) + 1) & ~1;
            bool ok = std::abs(info.seconds - duration) < 0.06 && video.frames == expected && result.frames == expected &&
                info.height == expected_height && info.width == expected_width && info.audio == source_info.audio && (!force_cpu || !result.gpu_input);
            report += label + ": " + std::to_string(steady_ms() - began) + " ms, " + std::to_string(video.frames) + "/" +
                std::to_string(expected) + " frames, " + std::to_string(info.seconds) + " s, " + std::to_string(info.width) + "x" + std::to_string(info.height) +
                ", audio " + (info.audio ? "yes" : "no") + (request.mic ? " (with microphone)" : "") + ", " + result.video_encoder + " from " + result.decoder +
                ", GPU input " + (result.gpu_input ? "active" : force_cpu ? "forced off" : "inactive/fallback") + (ok ? "" : "  MISMATCH") + "\n";
            if (!ok) ++failures;
        } catch (const std::exception& e) { report += label + " failed: " + e.what() + "\n"; ++failures; }
    };
    for (const char* codec : {"h264", "av1"}) {
        for (int fps : {60, 30}) run(codec, fps, 720, false, std::string(codec) + "-" + std::to_string(fps));
        run(codec, 60, source_info.height, false, std::string(codec) + "-same-60-gpu");
        run(codec, 60, source_info.height, true, std::string(codec) + "-same-60-cpu");
    }
    report += failures ? "FAIL\n" : "PASS\n";
    atomic_write(app_dir() / "export-test.txt", report);
    return failures;
}
}
