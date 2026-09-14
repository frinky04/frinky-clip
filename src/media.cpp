#include "media.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/error.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace clip {
static void check(int code, const char* context) {
    if (code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, message, sizeof(message));
    throw std::runtime_error(std::string(context) + ": " + message);
}
struct Input {
    AVFormatContext* value = nullptr;
    explicit Input(const fs::path& p) {
        check(avformat_open_input(&value, path_text(p).c_str(), nullptr, nullptr), "Open recording");
        int result = avformat_find_stream_info(value, nullptr);
        if (result < 0) { avformat_close_input(&value); check(result, "Read recording streams"); }
    }
    ~Input() { avformat_close_input(&value); }
};
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
void remux(const std::vector<fs::path>& segments, const fs::path& destination) {
    if (segments.empty()) throw std::runtime_error("No completed footage to save");
    fs::create_directories(destination.parent_path());
    fs::path temp = destination; temp += L".partial";
    AVFormatContext* out = nullptr;
    check(avformat_alloc_output_context2(&out, nullptr, "mp4", path_text(temp).c_str()), "Create clip");
    AVPacket* packet = av_packet_alloc();
    try {
        Input first(segments.front());
        for (unsigned i = 0; i < first.value->nb_streams; ++i) {
            auto* stream = avformat_new_stream(out, nullptr); if (!stream) throw std::bad_alloc();
            check(avcodec_parameters_copy(stream->codecpar, first.value->streams[i]->codecpar), "Copy stream settings");
            stream->codecpar->codec_tag = 0; stream->time_base = first.value->streams[i]->time_base;
        }
        check(avio_open(&out->pb, path_text(temp).c_str(), AVIO_FLAG_WRITE), "Open clip output");
        check(avformat_write_header(out, nullptr), "Write clip header");
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
                check(av_interleaved_write_frame(out, packet), "Write clip packet");
            }
            if (read_result != AVERROR_EOF) check(read_result, "Read segment");
        }
        check(av_write_trailer(out), "Finish clip");
        check(avio_closep(&out->pb), "Close clip");
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
Frame decode_frame(const fs::path& p, std::int64_t offset_ms, int width, bool keyframe_only) {
    Input in(p); int index = -1;
    for (unsigned i = 0; i < in.value->nb_streams; ++i) if (in.value->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { index = (int)i; break; }
    if (index < 0) throw std::runtime_error("No video stream: " + path_text(p));
    auto* stream = in.value->streams[index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) throw std::runtime_error("No decoder for the recording's video codec");
    AVCodecContext* ctx = avcodec_alloc_context3(codec); if (!ctx) throw std::bad_alloc();
    AVPacket* packet = av_packet_alloc(); AVFrame* frame = av_frame_alloc(), *best = av_frame_alloc();
    SwsContext* scaler = nullptr;
    auto cleanup = [&] { av_packet_free(&packet); av_frame_free(&frame); av_frame_free(&best); avcodec_free_context(&ctx); sws_freeContext(scaler); };
    try {
        check(avcodec_parameters_to_context(ctx, stream->codecpar), "Configure decoder");
        ctx->thread_count = 0; // Auto: libaom tile/frame threads make 1440p AV1 tolerable in software.
        if (keyframe_only) ctx->skip_frame = AVDISCARD_NONKEY;
        check(avcodec_open2(ctx, codec, nullptr), "Open decoder");
        std::int64_t target = av_rescale_q(offset_ms, AVRational{1, 1000}, stream->time_base);
        if (offset_ms > 0) av_seek_frame(in.value, index, target, AVSEEK_FLAG_BACKWARD);
        bool have = false, done = false;
        auto consider = [&](AVFrame* f) {
            std::int64_t pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE && pts > target && have) { done = true; return; }
            av_frame_unref(best); av_frame_ref(best, f); have = true;
            if (keyframe_only || (pts != AV_NOPTS_VALUE && pts >= target)) done = true;
        };
        while (!done) {
            int r = av_read_frame(in.value, packet);
            if (r < 0) { avcodec_send_packet(ctx, nullptr); }
            else if (packet->stream_index != index) { av_packet_unref(packet); continue; }
            else { avcodec_send_packet(ctx, packet); av_packet_unref(packet); }
            while (!done) {
                int rr = avcodec_receive_frame(ctx, frame);
                if (rr == AVERROR(EAGAIN)) break;
                if (rr == AVERROR_EOF || rr < 0) { done = true; break; }
                consider(frame); av_frame_unref(frame);
            }
            if (r < 0) break;
        }
        if (!have) throw std::runtime_error("No decodable frame at that position");
        int height = std::max(1, (int)std::lround((double)best->height * width / std::max(1, best->width)));
        scaler = sws_getContext(best->width, best->height, (AVPixelFormat)best->format, width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) throw std::runtime_error("Cannot scale decoded frame");
        Frame out; out.width = width; out.height = height; out.rgba.resize((size_t)width * height * 4);
        std::uint8_t* planes[1] = {out.rgba.data()}; int strides[1] = {width * 4};
        sws_scale(scaler, best->data, best->linesize, 0, best->height, planes, strides);
        std::int64_t pts = best->best_effort_timestamp == AV_NOPTS_VALUE ? best->pts : best->best_effort_timestamp;
        out.pts_ms = pts == AV_NOPTS_VALUE ? offset_ms : av_rescale_q(pts, stream->time_base, AVRational{1, 1000});
        cleanup(); return out;
    } catch (...) { cleanup(); throw; }
}
}
