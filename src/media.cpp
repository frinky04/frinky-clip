#include "media.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/error.h>
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

