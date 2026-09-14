// Isolated benchmark of the real editor. Never link this file with app.cpp.
#include "app.hpp"
#include "recorder.hpp"
#include "timeline.hpp"
#include "media.hpp"
#include <imgui.h>
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <utility>
extern "C" {
#include <libavutil/log.h>
}

namespace clip {
namespace {
int run_seconds = 12;
std::int64_t began = 0;
ImGuiKey held_key = ImGuiKey_None;
size_t next_event = 0;
std::vector<std::pair<double, ImGuiKey>> events;
std::vector<std::pair<double, std::string>> phases;
bool resized = false, hidden = false, restored = false;
void phase(double elapsed, const char* text) { phases.emplace_back(elapsed, text); }
}
App::App() { primary_ = true; }
App::~App() = default;
HWND App::recorder() const { return nullptr; }
void App::attach(HWND window) { window_ = window; }
void App::warm() {}
void App::start() {}
void App::stop() {}
void App::quit() { quitting_ = true; }
void App::observe(const std::string&, bool, std::int64_t, bool) { state_ = "paused"; }
bool App::handle_message(UINT message, WPARAM, LPARAM) {
    if (message == WM_CLOSE || message == QuitMessage) { quit(); return true; }
    return false;
}
bool App::tick() {
    if (!began) { began = steady_ms(); phase(0, "first_tick"); }
    const double elapsed = (steady_ms() - began) / 1000.0;
    if (quitting_ || elapsed >= run_seconds) { phase(elapsed, "exit"); return true; }
    auto& io = ImGui::GetIO();
    // Release on a separate UI frame; never inject system keyboard/mouse input.
    if (held_key != ImGuiKey_None) { io.AddKeyEvent(held_key, false); held_key = ImGuiKey_None; }
    else if (next_event < events.size() && elapsed >= events[next_event].first) {
        held_key = events[next_event++].second;
        io.AddKeyEvent(held_key, true);
        phase(elapsed, held_key == ImGuiKey_I ? "mark_in" : held_key == ImGuiKey_O ? "mark_out" : "toggle_play");
    }
    if (!resized && elapsed >= 8) {
        RECT rect{}; GetWindowRect(window_, &rect);
        SetWindowPos(window_, nullptr, 0, 0, rect.right - rect.left + 120, rect.bottom - rect.top + 80, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        resized = true; phase(elapsed, "resize");
    }
    if (!hidden && elapsed >= 9) { ShowWindow(window_, SW_HIDE); hidden = true; phase(elapsed, "hide"); }
    if (hidden && !restored && elapsed >= 9.5) { ShowWindow(window_, SW_SHOWNOACTIVATE); restored = true; phase(elapsed, "restore"); }
    return false;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace clip;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetDllDirectoryW(exe_dir().c_str()); av_log_set_level(AV_LOG_ERROR);
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    fs::path home; int result = 1;
    try {
        if (!argv || argc < 3 || argc > 4) throw std::runtime_error("Usage: editor-bench <fixture.mkv> <6|1800> [seconds=12]");
        const fs::path fixture = fs::absolute(argv[1]); const int count = std::stoi(argv[2]);
        if (count != 6 && count != 1800) throw std::runtime_error("Segment count must be 6 or 1800");
        if (argc == 4) run_seconds = std::stoi(argv[3]);
        if (run_seconds < 3 || run_seconds > 300) throw std::runtime_error("Seconds must be 3 through 300");
        if (!fs::is_regular_file(fixture)) throw std::runtime_error("Fixture is not a file");
        home = fs::absolute(fs::temp_directory_path() / (L"FrinkyClipEditorBench-" + wide(unique_id())));
        if (!fs::create_directory(home)) throw std::runtime_error("Private benchmark directory already exists");
        if (!SetEnvironmentVariableW(L"FRINKY_CLIP_HOME", home.c_str()) ||
            !SetEnvironmentVariableW(L"FRINKY_CLIP_UI_PERF", (home / "ui.csv").c_str())) throw std::runtime_error("Cannot set private benchmark environment");
        plain_write(home / "EDITOR_BENCH_ONLY.txt", "Private fixture links and editor benchmark output; no recording process.\n");
        fs::create_directories("test-output");
        plain_write(fs::absolute("test-output/editor-bench-last.txt"), path_text(home) + "\n");
        const auto extent = probe_video(fixture);
        if (extent.frames <= 0 || extent.fps_num <= 0 || extent.fps_den <= 0) throw std::runtime_error("Fixture has no valid video extent");
        const auto duration = (std::int64_t)std::llround(extent.frames * 1000.0 * extent.fps_den / extent.fps_num);
        if (duration < 2000) throw std::runtime_error("Fixture must contain at least two seconds of video");
        const auto levels = audio_levels(fixture); const auto coarse = downsample_levels(levels, CoarseBinMs);
        Config cfg; cfg.record_on_launch = false; cfg.storage = home / "Recordings";
        cfg.retention_minutes = std::max(120, (int)std::ceil((duration * count + 1000) / 60000.0));
        if (cfg.retention_minutes > 1440) throw std::runtime_error("Fixture history exceeds supported 24 hours");
        cfg.save();
        const auto folder = cfg.storage / "buffer" / "session-editor-bench"; fs::create_directories(folder);
        plain_write(cfg.storage / "buffer" / ".frinky-buffer", "Frinky Clip buffer v1\n");
        BufferMap map; const auto end = now_ms() - 1000; fs::path link_source;
        for (int i = 0; i < count; ++i) {
            // NTFS limits links per file. Private copies also leave the original
            // fixture's link count unchanged across repeated benchmark runs.
            if (i % 500 == 0) {
                link_source = home / ("fixture-" + std::to_string(i / 500) + ".mkv");
                fs::copy_file(fixture, link_source);
            }
            const auto path = folder / ("segment-" + std::to_string(i) + ".mkv"); std::error_code error;
            fs::create_hard_link(link_source, path, error);
            if (error) {
                if (count > 6) throw std::runtime_error("Hardlink failed; large benchmark refuses file copies: " + error.message());
                fs::copy_file(fixture, path);
            }
            const auto start = end - (std::int64_t)(count - i) * duration;
            Span span{path, "session-editor-bench", start, start + duration}; span.coarse = coarse; map.spans.push_back(std::move(span));
            // Peak-only sidecars: the benchmark index is the authoritative timeline.
            auto sidecar = data();
            obs_data_set_int(sidecar.get(), "video_frames", extent.frames); write_levels(sidecar.get(), levels);
            auto sidecar_path = path; sidecar_path += L".json"; write_json_fast(sidecar_path, sidecar.get());
        }
        // Keep the final edge near startup even when constructing 1800 sidecars took time.
        const auto shift = now_ms() - 1000 - end;
        for (auto& span : map.spans) { span.start_ms += shift; span.end_ms += shift; }
        map.last_end_ms = map.spans.back().end_ms; write_index(home / "segments.json", map);
        events = {{2.0, ImGuiKey_I}, {2.2, ImGuiKey_Space}, {2.7, ImGuiKey_O}};
        for (double t = 3; t < run_seconds - .5; t += .8) events.emplace_back(t, ImGuiKey_Space);
        result = run_ui();
        std::ofstream log(home / "phases.csv"); log << "elapsed_seconds,event\n";
        for (const auto& [time, event] : phases) log << time << ',' << event << '\n';
        plain_write(home / "result.txt", "exit=" + std::to_string(result) + "\nsegments=" + std::to_string(count) + "\n");
    } catch (const std::exception& error) {
        if (!home.empty()) { try { plain_write(home / "error.txt", error.what()); } catch (...) {} }
        OutputDebugStringA(error.what());
    }
    if (argv) LocalFree(argv); if (SUCCEEDED(com)) CoUninitialize(); return result;
}
