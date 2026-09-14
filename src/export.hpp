#pragma once
#include "timeline.hpp"
#include <atomic>
namespace clip {
// Re-encode [request.start_ms, request.end_ms] of `sources` (closed segments
// of one session in time order) into `output`, an MP4, through the linked
// FFmpeg libraries: D3D11VA decode with software fallback, NVENC encode (H.264
// falls back to libx264 when NVENC is unavailable), AAC audio. Frames are
// chosen by their timeline time, so the cut is frame-accurate and audio is
// trimmed to the sample. `progress`, when given, receives 0..1. Throws on
// failure; the output is verified and published atomically.
struct ExportResult { std::string video_encoder, decoder; std::int64_t frames = 0; bool gpu_input = false; /* Actual D3D11 input to NVENC. */ };
ExportResult export_clip(const std::vector<Span>& sources, const ExportRequest& request, const fs::path& output, std::atomic<double>* progress = nullptr);
// Headless check: export a short range across a segment seam from a buffer
// folder with each frame rate and input path, verify duration and frame count, and
// write export-test.txt in the app directory. Returns the failure count.
int export_test(const fs::path& buffer_root, int seconds);
}
