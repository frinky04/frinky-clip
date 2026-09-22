#include "export.hpp"
#include "player.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace clip;
namespace {
void require(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }
using Packet = std::shared_ptr<AVPacket>;
Packet packet() { return Packet(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); }); }
struct Frame {
    AVFrame* p = av_frame_alloc();
    ~Frame() { av_frame_free(&p); }
};
struct Encoder {
    AVCodecContext* ctx = nullptr;
    std::vector<Packet> packets;
    explicit Encoder(const char* name) {
        auto* codec = avcodec_find_encoder_by_name(name); require(codec != nullptr, "Fixture encoder unavailable");
        ctx = avcodec_alloc_context3(codec);
    }
    ~Encoder() { avcodec_free_context(&ctx); }
    void open() { av_check(avcodec_open2(ctx, ctx->codec, nullptr), "Open fixture encoder"); }
    void send(AVFrame* frame) {
        av_check(avcodec_send_frame(ctx, frame), "Encode fixture");
        for (;;) {
            auto p = packet(); int result = avcodec_receive_packet(ctx, p.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            av_check(result, "Read fixture packet"); packets.push_back(std::move(p));
        }
    }
};
constexpr int Seconds = 16;
constexpr std::int64_t Origin = 1000000;
struct Fixture {
    Encoder video{"libx264"}, desktop{"aac"}, mic{"aac"};
    explicit Fixture() {
        auto* v = video.ctx;
        v->width = 320; v->height = 180; v->pix_fmt = AV_PIX_FMT_YUV420P;
        v->time_base = {1, 60}; v->framerate = {60, 1}; v->gop_size = 60; v->max_b_frames = 0;
        v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set(v->priv_data, "preset", "ultrafast", 0); video.open();
        Frame vf; vf.p->width = v->width; vf.p->height = v->height; vf.p->format = v->pix_fmt;
        av_check(av_frame_get_buffer(vf.p, 32), "Allocate fixture video");
        for (int i = 0; i < Seconds * 60; ++i) {
            av_check(av_frame_make_writable(vf.p), "Prepare fixture video");
            for (int c = 0; c < 3; ++c) std::memset(vf.p->data[c], c ? 128 : 16 + i / 60, (size_t)vf.p->linesize[c] * (c ? v->height / 2 : v->height));
            vf.p->pts = i; video.send(vf.p);
        }
        video.send(nullptr);
        // A non-48 kHz microphone also checks the resampler clock and tail.
        make_audio(desktop, 48000, 431.0); make_audio(mic, 44100, 719.0);
    }
    void make_audio(Encoder& enc, int rate, double frequency) {
        auto* a = enc.ctx; a->sample_fmt = AV_SAMPLE_FMT_FLTP; a->sample_rate = rate; a->time_base = {1, rate};
        a->bit_rate = 192000; a->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; av_channel_layout_default(&a->ch_layout, 2); enc.open();
        Frame f; f.p->format = a->sample_fmt; f.p->sample_rate = rate; f.p->nb_samples = a->frame_size;
        av_channel_layout_copy(&f.p->ch_layout, &a->ch_layout); av_check(av_frame_get_buffer(f.p, 0), "Allocate fixture audio");
        for (int start = 0; start < Seconds * rate; start += a->frame_size) {
            av_check(av_frame_make_writable(f.p), "Prepare fixture audio"); f.p->pts = start;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < a->frame_size; ++i) {
                double t = (start + i) / double(rate);
                reinterpret_cast<float*>(f.p->data[c])[i] = (float)(0.14 * std::sin(6.283185307179586 * frequency * t + c * 0.2) +
                    0.07 * std::sin(6.283185307179586 * (frequency * 1.719 * t + 0.11 * t * t)));
            }
            enc.send(f.p);
        }
        enc.send(nullptr);
    }
    void mux(const fs::path& path, int begin, int end, bool microphone = true) {
        AVFormatContext* out = nullptr;
        av_check(avformat_alloc_output_context2(&out, nullptr, "matroska", path_text(path).c_str()), "Create fixture");
        struct Event { Packet p; int stream; std::int64_t time; };
        std::vector<Event> events;
        Encoder* encoders[] = {&video, &desktop, &mic};
        for (int k = 0; k < (microphone ? 3 : 2); ++k) {
            auto& enc = *encoders[k]; auto* stream = avformat_new_stream(out, nullptr);
            av_check(avcodec_parameters_from_context(stream->codecpar, enc.ctx), "Fixture parameters"); stream->time_base = enc.ctx->time_base;
            std::int64_t first = AV_NOPTS_VALUE;
            for (auto& original : enc.packets) {
                auto relative = original->pts - enc.packets.front()->pts;
                if (av_compare_ts(relative, enc.ctx->time_base, begin, AVRational{1, 1}) < 0 ||
                    av_compare_ts(relative, enc.ctx->time_base, end, AVRational{1, 1}) >= 0) continue;
                if (first == AV_NOPTS_VALUE) first = original->pts;
                auto copy = packet(); av_check(av_packet_ref(copy.get(), original.get()), "Copy fixture packet");
                copy->pts -= first; copy->dts -= first;
                events.push_back({copy, k, av_rescale_q(copy->dts, enc.ctx->time_base, AVRational{1, 1000000})});
            }
        }
        av_check(avio_open(&out->pb, path_text(path).c_str(), AVIO_FLAG_WRITE), "Open fixture");
        av_check(avformat_write_header(out, nullptr), "Fixture header");
        std::stable_sort(events.begin(), events.end(), [](auto& a, auto& b) { return a.time < b.time; });
        for (auto& e : events) {
            av_packet_rescale_ts(e.p.get(), encoders[e.stream]->ctx->time_base, out->streams[e.stream]->time_base);
            e.p->stream_index = e.stream; av_check(av_interleaved_write_frame(out, e.p.get()), "Write fixture");
        }
        av_check(av_write_trailer(out), "Fixture trailer"); avio_closep(&out->pb); avformat_free_context(out);
    }
};
std::vector<float> decode_audio(const fs::path& path) {
    AVFormatContext* in = nullptr; av_check(avformat_open_input(&in, path_text(path).c_str(), nullptr, nullptr), "Read test export");
    av_check(avformat_find_stream_info(in, nullptr), "Probe test export");
    int index = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0); av_check(index, "Find exported audio");
    auto* par = in->streams[index]->codecpar; auto* decoder = avcodec_find_decoder(par->codec_id);
    auto* ctx = avcodec_alloc_context3(decoder); avcodec_parameters_to_context(ctx, par);
    ctx->pkt_timebase = in->streams[index]->time_base; av_check(avcodec_open2(ctx, decoder, nullptr), "Decode test export");
    std::vector<float> samples; Frame frame; auto p = packet();
    auto drain = [&] {
        for (;;) {
            int result = avcodec_receive_frame(ctx, frame.p);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            av_check(result, "Read test audio"); require(frame.p->format == AV_SAMPLE_FMT_FLTP, "Expected float AAC samples");
            auto* data = reinterpret_cast<float*>(frame.p->data[0]); samples.insert(samples.end(), data, data + frame.p->nb_samples); av_frame_unref(frame.p);
        }
    };
    while (av_read_frame(in, p.get()) >= 0) {
        if (p->stream_index == index) { av_check(avcodec_send_packet(ctx, p.get()), "Send test audio"); drain(); }
        av_packet_unref(p.get());
    }
    avcodec_send_packet(ctx, nullptr); drain(); avcodec_free_context(&ctx); avformat_close_input(&in); return samples;
}
// Compare every 10 ms window, so a short seam dropout cannot hide inside a
// good average over the whole clip. AAC re-encoding allows small differences.
void compare(const std::vector<float>& actual, const std::vector<float>& reference, const char* label) {
    require(actual.size() >= 4800, "Audio unexpectedly short");
    double worst = 0;
    for (size_t i = 4800; i + 4800 < actual.size() && i + 480 < reference.size(); i += 480) {
        double error = 0;
        for (size_t j = 0; j < 480; ++j) { double d = actual[i + j] - reference[i + j]; error += d * d; }
        worst = std::max(worst, std::sqrt(error / 480));
    }
    std::cout << label << ": worst 10 ms RMS error " << worst << '\n';
    require(worst < 0.012, "Audio discontinuity or sample-clock drift in export");
}
}
int main(int argc, char** argv) {
    av_log_set_level(AV_LOG_ERROR);
    try {
        // Read-only source runner for reproducing an actual editor export.
        if (argc == 4) {
            auto map = read_index(fs::path(argv[1])); require(map.has_value(), "Read source index");
            auto request = read_export_request(fs::path(argv[2]));
            auto result = export_clip(export_sources(map->spans, request), request, fs::path(argv[3]));
            std::cout << "Exported " << result.frames << " frames\n"; return 0;
        }
        auto dir = fs::temp_directory_path() / ("FrinkyClipAudioTests-" + unique_id()); fs::create_directories(dir);
        std::cout << "Fixtures: " << path_text(dir) << '\n';
        Fixture fixture; fixture.mux(dir / "whole.mkv", 0, Seconds);
        std::vector<Span> split;
        for (int i = 0; i < Seconds / 4; ++i) {
            auto path = dir / ("segment-" + std::to_string(i) + ".mkv"); fixture.mux(path, i * 4, (i + 1) * 4);
            split.push_back({path, "session-test", Origin + i * 4000, Origin + (i + 1) * 4000});
        }
        std::vector<Span> whole{{dir / "whole.mkv", "session-test", Origin, Origin + Seconds * 1000}};
        for (int rate : {48000, 44100}) for (int mode = 0; mode < 3; ++mode) {
            float desktop = mode == 1 ? 0.f : 0.8f, mic = mode == 0 ? 0.f : 0.6f;
            auto reference = player_audio_samples_for_test(whole, rate, desktop, mic);
            auto playback = player_audio_samples_for_test(split, rate, desktop, mic);
            require(playback.size() == reference.size(), "Playback seams preserve every audio sample");
            require(playback == reference, "Playback seams preserve decoder overlap, resampling and mixed samples exactly");
            auto sought = player_audio_samples_for_test(split, rate, desktop, mic, true);
            require(sought.size() == playback.size(), "Seeking restores cold-start sample trimming");
            // AAC's noise-substitution state need not be bit-identical after
            // a decoder flush; stale resampler audio is much larger than this.
            double max_error = 0;
            for (size_t n = 0; n < sought.size(); ++n) max_error = std::max(max_error, std::abs(double(sought[n]) - playback[n]));
            require(max_error < 0.005, "Seeking clears old queued audio and resampler delay");
            std::cout << "playback " << rate << " Hz, mix " << mode << ": identical seam samples; seek peak error " << max_error << '\n';
        }
        ExportRequest r; r.height = 180; r.bitrate_kbps = 1000; r.start_ms = Origin + 250; r.end_ms = Origin + 15983;
        for (int mode = 0; mode < 3; ++mode) {
            r.audio = mode != 1; r.mic = mode != 0; r.desktop_gain = 0.8; r.mic_gain = 0.6;
            auto reference_path = dir / ("reference-" + std::to_string(mode) + ".mp4"); auto split_path = dir / ("split-" + std::to_string(mode) + ".mp4");
            export_clip(whole, r, reference_path); auto result = export_clip(split, r, split_path);
            require(result.frames == (r.end_ms - r.start_ms) * r.fps / 1000, "Audio changes preserve video frame count");
            auto reference = decode_audio(reference_path), actual = decode_audio(split_path);
            require(actual.size() == reference.size(), "Split export preserves sample count");
            compare(actual, reference, mode == 0 ? "desktop" : mode == 1 ? "resampled microphone" : "mixed tracks");
        }
        // An exact seam needs a predecessor to warm the decoder and a successor
        // for any AAC packet that straddles the out point.
        r.audio = true; r.mic = false; r.desktop_gain = 1; r.start_ms = Origin + 4000; r.end_ms = Origin + 8000;
        auto selected = export_sources(split, r); require(selected.size() == 3, "Audio read-ahead includes adjacent segments");
        export_clip(whole, r, dir / "cut-reference.mp4"); export_clip(selected, r, dir / "cut.mp4");
        compare(decode_audio(dir / "cut.mp4"), decode_audio(dir / "cut-reference.mp4"), "exact seam cut");
        r.start_ms -= 1; r.end_ms += 1;
        export_clip(whole, r, dir / "cut-reference.mp4"); export_clip(export_sources(split, r), r, dir / "cut.mp4");
        compare(decode_audio(dir / "cut.mp4"), decode_audio(dir / "cut-reference.mp4"), "one millisecond either side of a seam");
        r.start_ms += 1; r.end_ms -= 1;
        r.audio = false; require(export_sources(split, r).size() == 1, "Muted export needs no audio read-ahead");
        export_clip(export_sources(split, r), r, dir / "muted.mp4"); require(!inspect_media(dir / "muted.mp4").audio, "Muted export has no audio stream");
        auto separated = split; separated[0].session = "another-session";
        r.audio = true; require(export_sources(separated, r).size() == 2, "Read-ahead stays in the selected session");
        separated = split; separated[2].start_ms += 20; r.end_ms = Origin + 10000;
        require(export_sources(separated, r).empty(), "Do not join across missing footage");
        bool rejected = false;
        try { export_clip(separated, r, dir / "invalid.mp4"); } catch (const std::exception&) { rejected = true; }
        require(rejected && !fs::exists(dir / "invalid.mp4"), "Invalid source chain is rejected before publication");
        // A missing microphone is intentional silence, including when it is
        // missing at output creation or returns after a whole absent segment.
        auto missing = split;
        for (int i : {0, 2}) {
            auto path = dir / ("no-mic-" + std::to_string(i) + ".mkv"); fixture.mux(path, i * 4, (i + 1) * 4, false); missing[i].path = path;
        }
        r.audio = false; r.mic = true; r.mic_gain = 1; r.start_ms = Origin; r.end_ms = Origin + Seconds * 1000;
        export_clip(missing, r, dir / "missing-mic.mp4"); auto absent = decode_audio(dir / "missing-mic.mp4");
        require(absent.size() >= Seconds * 48000 && absent.size() < Seconds * 48000 + 1024, "Missing tracks preserve the full output duration");
        auto energy = [&](int second) {
            double sum = 0; for (int i = second * 48000; i < (second + 1) * 48000; ++i) sum += absent[i] * absent[i]; return std::sqrt(sum / 48000);
        };
        require(energy(1) < 0.0001 && energy(9) < 0.0001 && energy(5) > 0.08 && energy(13) > 0.08, "Missing microphone is silent and returns at its timeline position");
        r.start_ms = Origin + 15000; r.end_ms = Origin + 16000;
        export_clip(whole, r, dir / "tail-reference.mp4"); export_clip(split, r, dir / "tail.mp4");
        auto tail = decode_audio(dir / "tail.mp4"), tail_reference = decode_audio(dir / "tail-reference.mp4");
        require(tail.size() == tail_reference.size(), "Resampler tail preserves the cut duration");
        compare(tail, tail_reference, "last available segment");
        fs::remove_all(dir);
        std::cout << "Audio export continuity passed.\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
