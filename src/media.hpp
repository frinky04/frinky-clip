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
void av_check(int code, const char* context); // Throws with the FFmpeg error text when code < 0.
// The video track's extent, counted from its packets without decoding: the
// exact number of frames and the frame rate. Segment timelines chain on this
// rather than on the container duration, which includes the audio tail.
struct VideoExtent { std::int64_t frames = 0; int fps_num = 60, fps_den = 1; };
VideoExtent probe_video(const fs::path& path);
// The audio track's "peak file", as editors keep it: per bin, the peak
// sample amplitude and the RMS, both linear with full scale at 255. Peak
// draws the silhouette; RMS the solid core inside it. Empty when there is
// no audio. The recorder stores this in each segment's sidecar as hex text.
constexpr int AudioBinMs = 20;
struct AudioLevels { std::vector<std::uint8_t> peak, rms; int bin_ms = AudioBinMs; bool empty() const { return peak.empty(); } };
AudioLevels audio_levels(const fs::path& path, int bin_ms = AudioBinMs);
std::string encode_levels(const std::vector<std::uint8_t>& levels);
std::vector<std::uint8_t> decode_levels(const std::string& text);
// The next level of the chain: peak by max, RMS by root mean square, over
// each group of fine bins. Published in the segment index so the whole
// buffer draws at once without reading any sidecar.
constexpr int CoarseBinMs = 250;
AudioLevels downsample_levels(const AudioLevels& levels, int to_bin_ms);
// Sidecar and index representation: "audio_peak", "audio_rms" hex text and "audio_bin_ms".
AudioLevels read_levels(obs_data_t* data);
void write_levels(obs_data_t* data, const AudioLevels& levels);
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
