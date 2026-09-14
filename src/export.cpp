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
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace clip {
namespace {
constexpr std::int64_t Million = 1000000;
template <class T, void (*Free)(T**)> struct Owned {
    T* value = nullptr;
    Owned() = default; explicit Owned(T* v) : value(v) {}
    Owned(const Owned&) = delete; Owned& operator=(const Owned&) = delete;
    ~Owned() { Free(&value); }
    T* operator->() const { return value; } operator T*() const { return value; }
    void reset(T* v = nullptr) { Free(&value); value = v; }
};
using CodecContext = Owned<AVCodecContext, avcodec_free_context>;
using Frame = Owned<AVFrame, av_frame_free>;
using Packet = Owned<AVPacket, av_packet_free>;
void free_fifo(AVAudioFifo** f) { if (*f) { av_audio_fifo_free(*f); *f = nullptr; } }
void free_scaler(SwsContext** s) { sws_freeContext(*s); *s = nullptr; }
using Fifo = Owned<AVAudioFifo, free_fifo>;
using Resampler = Owned<SwrContext, swr_free>;
using Scaler = Owned<SwsContext, free_scaler>;
using Format = Owned<AVFormatContext, avformat_close_input>;
struct HwFailure {};

bool supports(const AVCodecContext* ctx, const AVCodec* codec, AVPixelFormat wanted) {
    const AVPixelFormat* formats = nullptr; int count = 0;
    if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void**)&formats, &count) < 0 || !formats) return true;
    for (int i = 0; i < count; ++i) if (formats[i] == wanted) return true;
    return false;
}
// Open the first usable video encoder for the request. NVENC first; H.264
// additionally has a software fallback so an export never depends on the GPU.
AVCodecContext* open_video_encoder(const ExportRequest& r, int src_w, int src_h, std::string& name_out) {
    int height = r.height & ~1, width = ((int)std::lround((double)src_w * height / std::max(1, src_h)) + 1) & ~1;
    std::vector<const char*> names = r.codec == "av1" ? std::vector<const char*>{"av1_nvenc"} : std::vector<const char*>{"h264_nvenc", "libx264"};
    std::string errors;
    for (auto name : names) {
        const AVCodec* codec = avcodec_find_encoder_by_name(name);
        if (!codec) { errors += std::string(name) + " missing; "; continue; }
        CodecContext ctx(avcodec_alloc_context3(codec)); if (!ctx) throw std::bad_alloc();
        ctx->width = width; ctx->height = height; ctx->time_base = {1, r.fps}; ctx->framerate = {r.fps, 1};
        ctx->pix_fmt = supports(ctx, codec, AV_PIX_FMT_NV12) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        ctx->bit_rate = (std::int64_t)r.bitrate_kbps * 1000; ctx->rc_max_rate = ctx->bit_rate * 3 / 2; ctx->rc_buffer_size = (int)std::min<std::int64_t>(ctx->bit_rate * 2, INT32_MAX);
        ctx->gop_size = r.fps * 2; ctx->color_range = AVCOL_RANGE_MPEG; ctx->colorspace = AVCOL_SPC_BT709;
        ctx->color_primaries = AVCOL_PRI_BT709; ctx->color_trc = AVCOL_TRC_BT709; ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* options = nullptr;
        if (std::strstr(name, "nvenc")) { av_dict_set(&options, "preset", "p5", 0); av_dict_set(&options, "rc", "vbr", 0); }
        else { av_dict_set(&options, "preset", "fast", 0); ctx->thread_count = 0; }
        if (r.codec != "av1") av_dict_set(&options, "profile", "high", 0);
        int result = avcodec_open2(ctx, codec, &options); av_dict_free(&options);
        if (result < 0) { char text[AV_ERROR_MAX_STRING_SIZE]; av_strerror(result, text, sizeof(text)); errors += std::string(name) + ": " + text + "; "; continue; }
        name_out = name; auto* raw = ctx.value; ctx.value = nullptr; return raw;
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
// One open segment: demuxer, decoders, and their conversion state.
struct Source {
    Format fmt; Span span; int vindex = -1, aindex = -1;
    CodecContext video, audio; HwBinding binding; bool hardware = false;
    Resampler swr; std::int64_t audio_base_us = -1, audio_samples = 0; // Timeline position of the source's audio.
};
struct Exporter {
    const ExportRequest& r; std::shared_ptr<HwDevice> hw; std::atomic<double>* progress;
    std::int64_t in_us, out_us, total_frames, total_samples = 0;
    AVFormatContext* out = nullptr; AVStream* vstream = nullptr; AVStream* astream = nullptr;
    CodecContext venc, aenc; Packet packet{av_packet_alloc()}; Frame decoded{av_frame_alloc()}, cpu{av_frame_alloc()}, scaled{av_frame_alloc()}, audio_frame{av_frame_alloc()};
    Scaler sws; int sws_src_w = 0, sws_src_h = 0, sws_fmt = -1;
    Fifo fifo; std::int64_t frames_out = 0, samples_out = 0, audio_pts = 0; bool video_done = false, audio_done = false;
    std::string decoder_name, encoder_name;
    Exporter(const ExportRequest& request, std::atomic<double>* p) : r(request), progress(p) {
        in_us = request.start_ms * 1000; out_us = request.end_ms * 1000;
        total_frames = std::max<std::int64_t>(1, (out_us - in_us) * request.fps / Million);
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
        // The encoder's input is NV12 or YUV420P at the export size; convert
        // whatever the decoder produced.
        AVFrame* picture = frame;
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
        picture->pts = frames_out; picture->color_range = AVCOL_RANGE_MPEG;
        av_check(avcodec_send_frame(venc, picture), "Encode video"); write(venc, vstream);
        if (progress) *progress = std::clamp((double)++frames_out / (double)total_frames, 0.0, 1.0);
        else ++frames_out;
        if (frames_out >= total_frames) video_done = true;
    }
    void encode_audio(bool flush) {
        int size = aenc->frame_size > 0 ? aenc->frame_size : 1024;
        while (av_audio_fifo_size(fifo) >= size || (flush && av_audio_fifo_size(fifo) > 0)) {
            int n = std::min(size, av_audio_fifo_size(fifo));
            av_frame_unref(audio_frame); audio_frame->nb_samples = n; audio_frame->format = aenc->sample_fmt; audio_frame->sample_rate = aenc->sample_rate;
            av_channel_layout_copy(&audio_frame->ch_layout, &aenc->ch_layout);
            av_check(av_frame_get_buffer(audio_frame, 0), "Allocate audio frame");
            if (av_audio_fifo_read(fifo, (void**)audio_frame->data, n) < n) throw std::runtime_error("Audio queue underrun");
            audio_frame->pts = audio_pts; audio_pts += n;
            av_check(avcodec_send_frame(aenc, audio_frame), "Encode audio"); write(aenc, astream);
        }
    }
    // Place decoded samples on the output sample grid: skip what is already
    // written, fill any gap with silence, stop at the out point.
    void place_audio(Source& src, AVFrame* frame) {
        if (src.audio_base_us < 0) {
            std::int64_t pts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) return;
            src.audio_base_us = src.span.start_ms * 1000 + av_rescale_q(pts, src.fmt->streams[src.aindex]->time_base, AVRational{1, Million});
            src.audio_samples = 0;
        }
        int rate = aenc->sample_rate;
        std::int64_t first = av_rescale(src.audio_base_us - in_us, rate, Million) + src.audio_samples; // Output index of this frame's first sample.
        src.audio_samples += frame->nb_samples;
        std::uint8_t* converted[AV_NUM_DATA_POINTERS] = {};
        int count = swr_get_out_samples(src.swr, frame->nb_samples); if (count <= 0) return;
        av_check(av_samples_alloc(converted, nullptr, aenc->ch_layout.nb_channels, count, aenc->sample_fmt, 0), "Convert audio");
        count = swr_convert(src.swr, converted, count, (const std::uint8_t**)frame->extended_data, frame->nb_samples);
        if (count > 0) {
            std::int64_t skip = std::clamp<std::int64_t>(samples_out - first, 0, count);
            std::int64_t gap = std::clamp<std::int64_t>(first - samples_out, 0, rate); // Bounded silence for a seam or dropout.
            if (gap > 0) {
                std::uint8_t* silence[AV_NUM_DATA_POINTERS] = {};
                av_check(av_samples_alloc(silence, nullptr, aenc->ch_layout.nb_channels, (int)gap, aenc->sample_fmt, 0), "Fill audio gap");
                av_samples_set_silence(silence, 0, (int)gap, aenc->ch_layout.nb_channels, aenc->sample_fmt);
                av_audio_fifo_write(fifo, (void**)silence, (int)gap); samples_out += gap; av_freep(&silence[0]);
            }
            std::int64_t take = std::min<std::int64_t>(count - skip, total_samples - samples_out);
            if (take > 0) {
                // Advance plane pointers past the skipped samples.
                std::uint8_t* planes[AV_NUM_DATA_POINTERS] = {}; int bytes = av_get_bytes_per_sample(aenc->sample_fmt);
                for (int c = 0; c < aenc->ch_layout.nb_channels; ++c) planes[c] = converted[c] + skip * bytes;
                av_audio_fifo_write(fifo, (void**)planes, (int)take); samples_out += take;
            }
            if (samples_out >= total_samples) audio_done = true;
        }
        av_freep(&converted[0]);
        encode_audio(false);
    }
    void open(Source& src, const Span& span, bool hardware) {
        src.fmt.reset(); src.video.reset(); src.audio.reset(); src.swr.reset(); src.audio_base_us = -1; src.vindex = src.aindex = -1;
        AVFormatContext* fmt = nullptr;
        if (!open_without_probe(&fmt, span.path)) throw std::runtime_error("Cannot open " + path_text(span.path));
        src.fmt.reset(fmt); src.span = span;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            auto* par = fmt->streams[i]->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && src.vindex < 0) src.vindex = (int)i;
            if (par->codec_type == AVMEDIA_TYPE_AUDIO && src.aindex < 0) src.aindex = (int)i;
        }
        if (src.vindex < 0) throw std::runtime_error("No video stream: " + path_text(span.path));
        auto* vpar = fmt->streams[src.vindex]->codecpar;
        src.hardware = hardware && hw != nullptr;
        const AVCodec* vcodec = pick_decoder(vpar->codec_id, src.hardware);
        if (!vcodec) throw std::runtime_error("No decoder for the recording's video codec");
        src.video.reset(avcodec_alloc_context3(vcodec)); av_check(avcodec_parameters_to_context(src.video, vpar), "Configure decoder");
        src.video->thread_count = src.hardware ? 1 : 0;
        if (src.hardware) { src.binding = {hw, false, 8}; src.video->opaque = &src.binding; src.video->get_format = hw_get_format; }
        av_check(avcodec_open2(src.video, vcodec, nullptr), "Open decoder");
        decoder_name = std::string(vcodec->name) + (src.hardware ? " (D3D11VA)" : " (software)");
        if (r.audio && src.aindex >= 0) {
            auto* apar = fmt->streams[src.aindex]->codecpar; const AVCodec* acodec = avcodec_find_decoder(apar->codec_id);
            if (acodec) {
                src.audio.reset(avcodec_alloc_context3(acodec));
                if (avcodec_parameters_to_context(src.audio, apar) < 0 || avcodec_open2(src.audio, acodec, nullptr) < 0) src.audio.reset();
            }
            if (src.audio && !aenc) { aenc.reset(open_audio_encoder(src.audio)); if (!aenc) src.audio.reset(); }
            if (src.audio) {
                SwrContext* swr = nullptr;
                if (swr_alloc_set_opts2(&swr, &aenc->ch_layout, aenc->sample_fmt, aenc->sample_rate, &src.audio->ch_layout, src.audio->sample_fmt, src.audio->sample_rate, 0, nullptr) < 0 || swr_init(swr) < 0) { swr_free(&swr); src.audio.reset(); }
                src.swr.reset(swr);
            }
        }
    }
    void begin_output(const fs::path& temp, int src_w, int src_h) {
        av_check(avformat_alloc_output_context2(&out, nullptr, "mp4", path_text(temp).c_str()), "Create clip");
        venc.reset(open_video_encoder(r, src_w, src_h, encoder_name));
        vstream = avformat_new_stream(out, nullptr); if (!vstream) throw std::bad_alloc();
        av_check(avcodec_parameters_from_context(vstream->codecpar, venc), "Describe video"); vstream->time_base = venc->time_base; vstream->avg_frame_rate = venc->framerate;
        if (aenc) {
            astream = avformat_new_stream(out, nullptr); if (!astream) throw std::bad_alloc();
            av_check(avcodec_parameters_from_context(astream->codecpar, aenc), "Describe audio"); astream->time_base = aenc->time_base;
            fifo.reset(av_audio_fifo_alloc(aenc->sample_fmt, aenc->ch_layout.nb_channels, aenc->frame_size > 0 ? aenc->frame_size * 4 : 4096));
            total_samples = av_rescale(out_us - in_us, aenc->sample_rate, Million);
        } else audio_done = true;
        av_check(avio_open(&out->pb, path_text(temp).c_str(), AVIO_FLAG_WRITE), "Open clip output");
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
            if (!out) begin_output_from(f);
            // Emit this picture for every output slot it covers; a source
            // frame ahead of the next slot is dropped (60 to 30 fps).
            while (!video_done && due_us(frames_out) <= t + 1000) encode_video(f);
        };
        auto drain = [&](AVCodecContext* ctx, bool video) {
            while (true) {
                int result = avcodec_receive_frame(ctx, decoded);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
                if (result < 0) { if (video && src.hardware && !frames_seen) throw HwFailure{}; av_check(result, "Decode"); }
                if (video) video_frame(decoded); else if (!audio_done && aenc) place_audio(src, decoded);
                av_frame_unref(decoded);
            }
        };
        bool eof = false;
        while (!(video_done && audio_done)) {
            int result = eof ? AVERROR_EOF : av_read_frame(src.fmt, packet);
            if (result < 0) {
                if (result != AVERROR_EOF) av_check(result, "Read segment");
                if (eof) break;
                eof = true; avcodec_send_packet(src.video, nullptr); if (src.audio) avcodec_send_packet(src.audio, nullptr);
                drain(src.video, true); if (src.audio) drain(src.audio, false);
                break;
            }
            if (packet->stream_index == src.vindex) {
                int sent = avcodec_send_packet(src.video, packet); av_packet_unref(packet);
                if (sent < 0 && sent != AVERROR(EAGAIN)) { if (src.hardware && !frames_seen) throw HwFailure{}; av_check(sent, "Decode video"); }
                drain(src.video, true);
            } else if (src.audio && packet->stream_index == src.aindex) {
                avcodec_send_packet(src.audio, packet); av_packet_unref(packet);
                drain(src.audio, false);
            } else av_packet_unref(packet);
        }
        return !(video_done && audio_done);
    }
    fs::path temp_path;
    void begin_output_from(AVFrame* f) { begin_output(temp_path, f->width, f->height); }
    void finish() {
        if (!out) throw std::runtime_error("No frames in the marked range");
        av_check(avcodec_send_frame(venc, nullptr), "Flush video"); write(venc, vstream);
        if (aenc) { encode_audio(true); av_check(avcodec_send_frame(aenc, nullptr), "Flush audio"); write(aenc, astream); }
        av_check(av_write_trailer(out), "Finish clip"); av_check(avio_closep(&out->pb), "Close clip");
    }
};
}

ExportResult export_clip(const std::vector<Span>& sources, const ExportRequest& request, const fs::path& output, std::atomic<double>* progress) {
    if (sources.empty()) throw std::runtime_error("No footage in the marked range");
    if (request.end_ms - request.start_ms < 100) throw std::runtime_error("Mark a range of at least 0.1 s.");
    if (request.fps <= 0 || request.height <= 0 || request.bitrate_kbps <= 0) throw std::runtime_error("Invalid export settings");
    fs::create_directories(output.parent_path());
    fs::path temp = output; temp += L".partial";
    auto hw = hw_device(nullptr);
    ExportResult result;
    for (bool hardware : {true, false}) {
        if (hardware && !hw) continue;
        std::error_code ec; fs::remove(temp, ec);
        Exporter exporter(request, progress); exporter.hw = hw; exporter.temp_path = temp;
        try {
            Source src;
            for (auto& span : sources) {
                if (span.end_ms * 1000 <= exporter.in_us) continue; // Entirely before the in point.
                exporter.open(src, span, hardware);
                if (!exporter.run_source(src)) break;
            }
            exporter.finish();
            result.video_encoder = exporter.encoder_name; result.decoder = exporter.decoder_name; result.frames = exporter.frames_out;
            break;
        } catch (const HwFailure&) {
            // The device cannot decode this stream: redo the export in software.
            if (!hardware) throw std::runtime_error("Cannot decode the recording");
            continue;
        }
    }
    inspect_media(temp);
    if (!flush_closed(temp)) throw std::runtime_error("Could not flush exported clip to disk");
    if (!MoveFileExW(temp.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING)) throw std::runtime_error("Could not publish exported clip");
    if (progress) *progress = 1.0;
    return result;
}

int export_test(const fs::path& buffer_root, int seconds) {
    std::string report; int failures = 0;
    auto map = scan_buffer(buffer_root);
    report += "segments: " + std::to_string(map.spans.size()) + "\n";
    // The range straddles the last exact seam so both segment hand-off and
    // in-segment trimming are exercised.
    size_t seam = map.spans.size();
    for (size_t i = map.spans.size(); i > 1; --i) if (map.spans[i - 1].start_ms == map.spans[i - 2].end_ms) { seam = i - 1; break; }
    if (seam >= map.spans.size()) { atomic_write(app_dir() / "export-test.txt", report + "No contiguous segment pair\n"); return 1; }
    ExportRequest request; request.start_ms = map.spans[seam].start_ms - 1500; request.end_ms = request.start_ms + seconds * 1000;
    request.height = 720; request.bitrate_kbps = 8000;
    for (const char* codec : {"h264", "av1"}) for (int fps : {60, 30}) {
        request.codec = codec; request.fps = fps;
        auto sources = spans_in_range(map.spans, request.start_ms, request.end_ms);
        auto output = app_dir() / ("export-test-" + std::string(codec) + "-" + std::to_string(fps) + ".mp4");
        auto began = now_ms();
        try {
            auto result = export_clip(sources, request, output);
            auto info = inspect_media(output); auto video = probe_video(output);
            std::int64_t expected = (std::int64_t)seconds * fps;
            bool ok = std::abs(info.seconds - seconds) < 0.06 && video.frames == expected && info.height == 720 && info.audio;
            report += std::string(codec) + " " + std::to_string(fps) + " fps: " + std::to_string(now_ms() - began) + " ms, " + std::to_string(video.frames) + "/" +
                std::to_string(expected) + " frames, " + std::to_string(info.seconds) + " s, " + std::to_string(info.width) + "x" + std::to_string(info.height) +
                ", audio " + (info.audio ? "yes" : "no") + ", " + result.video_encoder + " from " + result.decoder + (ok ? "" : "  MISMATCH") + "\n";
            if (!ok) ++failures;
        } catch (const std::exception& e) { report += std::string(codec) + " " + std::to_string(fps) + " fps failed: " + e.what() + "\n"; ++failures; }
    }
    report += failures ? "FAIL\n" : "PASS\n";
    atomic_write(app_dir() / "export-test.txt", report);
    return failures;
}
}
