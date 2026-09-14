#pragma once
#include "common.hpp"
#include <memory>
extern "C" {
#include <libavutil/pixfmt.h>
}
struct AVBufferRef; struct AVCodecContext; struct AVCodec; struct AVFrame; struct AVFormatContext; struct ID3D11Device;
namespace clip {
struct MediaInfo { double seconds = 0; int width = 0, height = 0; bool audio = false; };
MediaInfo inspect_media(const fs::path& path);
void remux(const std::vector<fs::path>& segments, const fs::path& destination);
// A D3D11VA decode device shared by decoders. Wrapping the render device lets
// decoded frames be sampled directly as shader resources; a private device
// serves decoders that download frames to the CPU.
struct HwDevice { AVBufferRef* ref = nullptr; ~HwDevice(); };
std::shared_ptr<HwDevice> hw_device(ID3D11Device* render_device);
std::string adapter_name(ID3D11Device* device); // GPU description, for diagnostics.
std::string adapter_name(const HwDevice& device);
// Open a container, probing streams only when the headers are incomplete.
bool open_without_probe(AVFormatContext** fmt, const fs::path& path);
// Set as AVCodecContext::opaque with get_format = hw_get_format. The decoder
// then runs on the device when the codec has a D3D11VA hwaccel and falls back
// to software otherwise. extra_frames enlarges the surface pool for frames
// the caller keeps referenced.
struct HwBinding { std::shared_ptr<HwDevice> device; bool shader_resource = false; int extra_frames = 8; };
AVPixelFormat hw_get_format(AVCodecContext* ctx, const AVPixelFormat* formats);
// The decoder to use for a codec. For AV1 the registered default is libaom,
// a software decoder that never offers hardware formats; the native "av1"
// decoder is the one with the D3D11VA hwaccel, so it is chosen when hardware
// decoding is wanted.
const AVCodec* pick_decoder(int codec_id, bool hardware);
// A decoded picture scaled to `width` pixels, RGBA8, row-major.
struct Frame { int width = 0, height = 0; std::vector<std::uint8_t> rgba; std::int64_t pts_ms = 0; };
// Decode from `path` the last frame at or before `offset_ms` into the file.
// With `keyframe_only`, return the keyframe at or before it instead, which is
// much cheaper and what the timeline thumbnails use.
Frame decode_frame(const fs::path& path, std::int64_t offset_ms, int width, bool keyframe_only, std::shared_ptr<HwDevice> hw = {});
// A reusable single-picture decoder: the codec context, its hardware surface
// pool, and the scaler survive between calls, so only the first decode of a
// stream pays the setup cost. Not thread-safe.
class Decoder {
public:
    explicit Decoder(std::shared_ptr<HwDevice> hw = {});
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Frame decode(const fs::path& path, std::int64_t offset_ms, int width, bool keyframe_only);
private:
    struct State; State* s_;
    friend Frame decode_with(State*, const fs::path&, std::int64_t, int, bool);
};
}
