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
        check(avformat_open_input(&value, path_text(p).c_str(), nullptr, nullptr), "Open recording");
        if (probe || !headers_complete(value)) {
            int result = avformat_find_stream_info(value, nullptr);
            if (result < 0) { avformat_close_input(&value); check(result, "Read recording streams"); }
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
        check(avcodec_parameters_to_context(ctx, par), "Configure decoder");
        // Hardware decoding runs asynchronously on the GPU; frame threads only add per-thread setup.
        ctx->thread_count = hardware ? 1 : 0;
        if (hardware) { binding = {hw, false, 16}; ctx->opaque = &binding; ctx->get_format = hw_get_format; }
        check(avcodec_open2(ctx, codec, nullptr), "Open decoder");
        codec_id = par->codec_id; width = par->width; height = par->height;
        extradata.assign(par->extradata, par->extradata + par->extradata_size);
    }
};
Decoder::Decoder(std::shared_ptr<HwDevice> hw) : s_(new State) { s_->hw = std::move(hw); }
Decoder::~Decoder() { delete s_; }
struct HwFailure {};
Frame decode_with(Decoder::State* s, const fs::path& p, std::int64_t offset_ms, int width, bool keyframe_only);
Frame Decoder::decode(const fs::path& p, std::int64_t offset_ms, int width, bool keyframe_only) {
    try { return decode_with(s_, p, offset_ms, width, keyframe_only); }
    catch (const HwFailure&) {
        // The native decoder could not use the device: fall back to software for good.
        s_->hw_failed = true; avcodec_free_context(&s_->ctx);
        return decode_with(s_, p, offset_ms, width, keyframe_only);
    }
}
Frame decode_with(Decoder::State* s_, const fs::path& p, std::int64_t offset_ms, int width, bool keyframe_only) {
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
        int r = av_read_frame(in.value, packet);
        int sent = 0;
        if (r < 0) { avcodec_send_packet(ctx, nullptr); }
        else if (packet->stream_index != index) { av_packet_unref(packet); continue; }
        else { sent = avcodec_send_packet(ctx, packet); av_packet_unref(packet); }
        if (sent < 0 && sent != AVERROR(EAGAIN) && hardware && !have) throw HwFailure{};
        while (!done) {
            int rr = avcodec_receive_frame(ctx, frame);
            if (rr == AVERROR(EAGAIN)) break;
            if (rr == AVERROR_EOF) { done = true; break; }
            if (rr < 0) { if (hardware && !have) throw HwFailure{}; done = true; break; }
            consider(frame); av_frame_unref(frame);
        }
        if (r < 0) break;
    }
    if (!have) throw std::runtime_error("No decodable frame at that position");
    AVFrame* picture = best;
    if (best->format == AV_PIX_FMT_D3D11) { av_frame_unref(s_->cpu); check(av_hwframe_transfer_data(s_->cpu, best, 0), "Download decoded frame"); picture = s_->cpu; }
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
