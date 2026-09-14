#pragma once
#include "timeline.hpp"
#include "media.hpp"
#include <d3d11.h>
#include <imgui.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace clip {
// Plays the buffer from any point: a decode thread walks the segment files in
// order on the GPU (D3D11VA, software fallback), and feeds desktop audio to
// WASAPI. Audio buffering paces playback; the UI shows the frame due at the
// playhead, converting NV12 to RGB with a shader so pixels never cross to the
// CPU. Pause keeps the decoder and queue warm so resume is instant.
class Player {
public:
    Player(ID3D11Device* device, ID3D11DeviceContext* context);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    void set_spans(std::vector<Span> spans); // Closed segments in time order.
    void seek(std::int64_t epoch_ms);        // Show this frame; preserve playback state.
    void set_range(std::int64_t in_ms, std::int64_t out_ms); // Incomplete/zero clears it; edits pause without seeking.
    void play();
    void pause();
    void toggle() { playing() ? pause() : play(); }
    bool playing() const { return playing_; }
    std::int64_t position() const;           // Playhead in epoch ms.
    void set_end(std::int64_t epoch_ms) { end_ms_ = epoch_ms; } // Last playable time.
    void set_width(int pixels) { width_ = pixels; }
    // Playback mix: desktop and microphone tracks at these gains (1 = unity, 0 = muted).
    void set_gains(float desktop, float mic) { gains_[0] = desktop; gains_[1] = mic; }
    struct Picture { ImTextureID texture = 0; int width = 0, height = 0; std::int64_t ms = 0; };
    Picture tick(); // Main thread: present the frame due now.
    bool busy() const { return playing_ || pending_seek_ || seeking_; }
    bool seeking() const { return pending_seek_ || seeking_; } // Catching up to a seek target.
    bool hardware() const { return hardware_; }
    std::string error() const { std::lock_guard lock(mutex_); return error_; }
private:
    friend int player_test(const fs::path&, int);
    std::atomic<bool> fail_audio_for_test_{false};
    void request_seek(std::int64_t ms); // Caller holds mutex_.
    std::int64_t playback_end() const;
    struct Decoded { std::int64_t ms = 0; int width = 0, height = 0; std::vector<std::uint8_t> rgba; std::shared_ptr<AVFrame> hw; };
    struct Source;
    void work();
    bool open(Source& src, const Span& span, std::int64_t offset_ms);
    bool reposition(Source& src, std::int64_t offset_ms); // Seek within the open segment.
    bool step(Source& src, Decoded* out_video, bool want_audio);
    void push_audio(Source& src, size_t track, void* frame);
    void mix_audio(Source& src, bool flush); // Sum the tracks in step onto the device queue.
    bool convert_setup();
    void present(const Decoded& frame);
    ID3D11Device* device_; ID3D11DeviceContext* context_;
    std::shared_ptr<HwDevice> hw_;
    // Presentation resources: an RGBA target the NV12 shader renders into, or
    // a dynamic texture for software frames.
    ID3D11Texture2D* texture_ = nullptr; ID3D11ShaderResourceView* view_ = nullptr; ID3D11RenderTargetView* target_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0; bool tex_dynamic_ = false;
    ID3D11VertexShader* vs_ = nullptr; ID3D11PixelShader* ps_ = nullptr; ID3D11SamplerState* sampler_ = nullptr; bool convert_ready_ = false;
    std::shared_ptr<const std::vector<Span>> spans_ = std::make_shared<const std::vector<Span>>();
    std::deque<Decoded> ready_;
    std::optional<Decoded> shown_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, space_;
    std::thread worker_;
    std::atomic<bool> stop_{false}, playing_{false}, pending_seek_{false}, seeking_{false}, seek_result_{false}, resumable_{false}, hardware_{false};
    bool hw_ok_ = true; // Cleared when the native decoder cannot use the device.
    std::atomic<int> width_{640};
    std::atomic<float> gains_[2]{1.f, 1.f};
    std::atomic<std::int64_t> position_{0}, end_ms_{0}, seek_target_{0};
    std::atomic<std::int64_t> range_start_{0}, range_end_{0}, drain_end_ms_{0};
    bool restart_decoder_ = false; // Range edits and cached seeks invalidate queued audio.
    std::atomic<std::uint64_t> generation_{0};
    // Playback clock: media time at the moment audio (or wall time) started.
    std::atomic<std::int64_t> clock_media_{0}, clock_wall_{0};
    std::string error_;
    mutable std::mutex audio_clock_mutex_;
    std::int64_t audio_time_ = -1; // Published by worker; never exposes WASAPI to the UI.
    struct Audio; Audio* audio_ = nullptr; // Worker thread only.
};
}
