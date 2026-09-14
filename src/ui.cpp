#include "recorder.hpp"
#include "app.hpp"
#include "updates.hpp"
#include "version.h"
#include "resource.h"
#include "timeline.hpp"
#include "thumbs.hpp"
#include "waveform.hpp"
#include "player.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <climits>
#include <future>
#include <fstream>
#include <optional>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <system_error>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace clip {
namespace {
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
IDXGISwapChain* swapchain = nullptr;
ID3D11RenderTargetView* target = nullptr;
ImVec4 rgb(unsigned v) { return ImVec4(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, 1); }
const ImVec4 background = rgb(0x0b0c0e), accent = rgb(0x7fa7cf), muted = rgb(0x9ba1ab), foreground = rgb(0xe2e4e8);
void theme(float dpi) {
    auto& s = ImGui::GetStyle(); s = ImGuiStyle(); ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(14, 10); s.FramePadding = ImVec2(7, 3);
    s.ItemSpacing = ImVec2(6, 4); s.ItemInnerSpacing = ImVec2(5, 3); s.CellPadding = ImVec2(0, 2);
    s.WindowRounding = 0; s.ChildRounding = 0; s.FrameRounding = 2; s.PopupRounding = 2; s.GrabRounding = 1;
    s.WindowBorderSize = 0; s.FrameBorderSize = 1; s.PopupBorderSize = 1; s.ScrollbarSize = 11; s.ScrollbarRounding = 1;
    s.DisabledAlpha = .62f; s.SeparatorTextPadding = ImVec2(0, 3); s.SeparatorTextBorderSize = 1;
    auto* c = s.Colors;
    c[ImGuiCol_Text] = foreground; c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_WindowBg] = background; c[ImGuiCol_ChildBg] = background; c[ImGuiCol_PopupBg] = rgb(0x131518);
    c[ImGuiCol_Border] = rgb(0x2b2f35); c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = rgb(0x131518); c[ImGuiCol_FrameBgHovered] = rgb(0x1e2227); c[ImGuiCol_FrameBgActive] = rgb(0x292e35);
    c[ImGuiCol_Button] = rgb(0x1b1e22); c[ImGuiCol_ButtonHovered] = rgb(0x2a2f36); c[ImGuiCol_ButtonActive] = rgb(0x363d46);
    c[ImGuiCol_Header] = rgb(0x1c2733); c[ImGuiCol_HeaderHovered] = rgb(0x2a3b4d); c[ImGuiCol_HeaderActive] = rgb(0x344b63);
    c[ImGuiCol_CheckMark] = accent; c[ImGuiCol_SliderGrab] = accent; c[ImGuiCol_SliderGrabActive] = rgb(0x9cbbdc);
    c[ImGuiCol_Separator] = rgb(0x292d33); c[ImGuiCol_SeparatorHovered] = accent; c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = rgb(0x292d33); c[ImGuiCol_ResizeGripHovered] = accent; c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_ScrollbarBg] = background; c[ImGuiCol_ScrollbarGrab] = rgb(0x30353d); c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x464e59); c[ImGuiCol_ScrollbarGrabActive] = rgb(0x565e6a);
    c[ImGuiCol_PlotHistogram] = accent; c[ImGuiCol_TextSelectedBg] = ImVec4(.50f, .65f, .81f, .28f); c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_TitleBg] = rgb(0x1b1e22); c[ImGuiCol_TitleBgActive] = rgb(0x1b1e22); c[ImGuiCol_TitleBgCollapsed] = rgb(0x1b1e22);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, .55f);
    s.ScaleAllSizes(dpi); s.FontScaleDpi = dpi;
}
// Window sizing, in 100%-scale client pixels. The chrome is everything but
// the viewport: header, overview, lanes, transport and export rows, footer
// and its message line. The default window makes the 16:9 viewport fill the
// content width; the minimum keeps the viewport at its smallest usable
// height so nothing scrolls off the bottom.
constexpr int DefaultClientWidth = 760, MinClientWidth = 720, ChromeHeight = 327, MinPreviewHeight = 96, SidePadding = 14;
RECT window_rect(int client_w, int client_h, UINT dpi) {
    float scale = dpi / 96.f; RECT rect{0, 0, (LONG)(client_w * scale), (LONG)(client_h * scale)};
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi); return rect;
}
void render_target() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) { device->CreateRenderTargetView(back, nullptr, &target); back->Release(); }
}
LRESULT CALLBACK ui_proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (msg == WM_QUERYENDSESSION) return TRUE;
    if (app && app->handle_message(msg, w, l)) return 0;
    if (ImGui_ImplWin32_WndProcHandler(window, msg, w, l)) return 1;
    if (msg == WM_SIZE && device && w != SIZE_MINIMIZED) {
        if (target) { target->Release(); target = nullptr; }
        swapchain->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0); render_target(); return 0;
    }
    if (msg == WM_DPICHANGED) {
        auto* rect = reinterpret_cast<RECT*>(l);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0;
    }
    if (msg == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(l);
        auto rect = window_rect(MinClientWidth, ChromeHeight + MinPreviewHeight, GetDpiForWindow(window));
        limits->ptMinTrackSize = {rect.right - rect.left, rect.bottom - rect.top}; return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, msg, w, l);
}
void open_path(HWND owner, fs::path path, std::string& error, bool folder = false) {
    try {
        path = fs::absolute(path).lexically_normal().make_preferred();
        std::wstring executable = path.wstring(), arguments;
        if (folder) {
            fs::create_directories(path);
            wchar_t windows[MAX_PATH];
            if (!GetWindowsDirectoryW(windows, MAX_PATH))
                throw std::system_error(GetLastError(), std::system_category(), "Cannot locate Explorer");
            executable = (fs::path(windows) / L"explorer.exe").wstring();
            arguments = quote_arg(path.wstring());
        }
        SHELLEXECUTEINFOW action{sizeof(action)};
        action.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI; action.hwnd = owner;
        action.lpVerb = L"open"; action.lpFile = executable.c_str();
        action.lpParameters = folder ? arguments.c_str() : nullptr; action.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&action))
            throw std::system_error(GetLastError(), std::system_category(), "Cannot open " + path_text(path));
        error.clear();
    } catch (const std::exception& e) { error = e.what(); }
}
std::string duration(double seconds) { int s = (int)seconds; char t[40]; snprintf(t, sizeof(t), "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60); return t; }
void help(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26); ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
}
// Right-click an adjustable value to restore its default.
template <class T> bool reset_to(T& value, T fallback) {
    if (!(ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) || value == fallback) return false;
    value = fallback; return true;
}
void section(const char* title) { ImGui::PushStyleColor(ImGuiCol_Text, muted); ImGui::SeparatorText(title); ImGui::PopStyleColor(); }
bool properties(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) return false;
    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.5f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch); return true;
}
void row(const char* label) {
    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-1);
}
bool bitrate_row(const char* name, int& kbps, bool& commit, int fallback) {
    row(name); ImGui::PushID(name); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
    float mbps = kbps / 1000.f; bool changed = ImGui::InputFloat("##value", &mbps, 0, 0, "%.1f");
    commit |= ImGui::IsItemDeactivatedAfterEdit();
    if (changed && std::isfinite(mbps) && mbps >= 0 && mbps <= 1000) kbps = (int)std::round(mbps * 1000);
    if (reset_to(kbps, fallback)) changed = commit = true;
    ImGui::SameLine(); ImGui::TextDisabled("Mbps"); ImGui::PopID(); return changed;
}
bool int_row(const char* label, const char* id, int& value, const char* unit, bool& commit, int fallback) {
    row(label); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
    bool changed = ImGui::InputInt(id, &value, 0, 0); commit |= ImGui::IsItemDeactivatedAfterEdit();
    if (reset_to(value, fallback)) changed = commit = true;
    ImGui::SameLine(); ImGui::TextDisabled("%s", unit); return changed;
}
bool primary_button(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, accent); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(0x9cbbdc));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(0x648cb4)); ImGui::PushStyleColor(ImGuiCol_Text, rgb(0x17191c));
    bool pressed = ImGui::Button(label, size); ImGui::PopStyleColor(4); return pressed;
}
std::string shortcut(const Config& c) {
    return std::string(c.modifiers & MOD_CONTROL ? "Ctrl+" : "") + (c.modifiers & MOD_SHIFT ? "Shift+" : "") +
        (c.modifiers & MOD_ALT ? "Alt+" : "") + "F" + std::to_string(c.hotkey - VK_F1 + 1);
}
}
int run_ui(int resume_recording, bool start_hidden) {
    App app;
    if (!app.primary()) {
        if (!start_hidden) if (auto existing = FindWindowW(AppWindowClass, nullptr)) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        return 0;
    }
    Updates updates;
    bool update_resume_recording = false;
    Config cfg = Config::load(); const Config defaults; auto displays = monitors(); auto microphones = capture_devices();
    char storage[2048]; strncpy_s(storage, path_text(cfg.storage).c_str(), _TRUNCATE);
    auto status = read_json(app_dir() / "status.json"); std::int64_t last_read = 0;
    std::string error, settings_error; bool dirty = false;
    if (updates.status().installed && !cfg.windows_startup_initialized) {
        try {
            set_starts_with_windows(true);
            cfg.windows_startup_initialized = true; cfg.save();
        } catch (const std::exception& e) { error = e.what(); }
    }
    WNDCLASSW wc{}; wc.style = CS_CLASSDC; wc.lpfnWndProc = ui_proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_FRINKY_CLIP)); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = AppWindowClass; RegisterClassW(&wc);
    auto initial = window_rect(DefaultClientWidth, (DefaultClientWidth - 2 * SidePadding) * 9 / 16 + ChromeHeight, GetDpiForSystem());
    HWND window = CreateWindowW(AppWindowClass, L"Frinky Clip", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        initial.right - initial.left, initial.bottom - initial.top, nullptr, nullptr, wc.hInstance, &app);
    if (!window) throw std::runtime_error("Cannot create app window");
    app.attach(window);
    BOOL dark = TRUE; DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF caption = RGB(11, 12, 14), text = RGB(226, 228, 232), border = RGB(43, 47, 53);
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &text, sizeof(text)); DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = window; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swapchain, &device, &level, &context)))
        throw std::runtime_error("Could not initialize the control window's Direct3D device");
    render_target(); IMGUI_CHECKVERSION(); ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    auto font_path = exe_dir() / "NotoSansMono-Medium.ttf";
    ImFont* regular = fs::exists(font_path) ? io.Fonts->AddFontFromFileTTF(path_text(font_path).c_str(), 16) : nullptr;
    if (!regular) regular = io.Fonts->AddFontDefault();
    io.FontDefault = regular;
    float dpi = GetDpiForWindow(window) / 96.f; theme(dpi);
    ImGui_ImplWin32_Init(window); ImGui_ImplDX11_Init(device, context);
    if (!start_hidden) {
        ShowWindow(window, SW_SHOW); SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW); UpdateWindow(window);
    }
    auto save_settings = [&] {
        try {
            cfg.storage = fs::path(wide(storage)); cfg.save(); dirty = false; settings_error.clear();
            return true;
        } catch (const std::exception& e) { settings_error = e.what(); return false; }
    };
    auto start_recording = [&] {
        if (!save_settings()) return;
        try { app.start(); error.clear(); } catch (const std::exception& e) { error = e.what(); }
    };
    auto request_update = [&] {
        if (!updates.status().ready || (!app.recording() && !app.paused()) || !save_settings()) return;
        update_resume_recording = app.recording(); app.begin_update();
    };
    // The recorder process is launched once and stays warm; without auto-record
    // it idles with the OBS core initialized so the first Record is quick.
    if (resume_recording < 0 ? cfg.record_on_launch : resume_recording != 0) start_recording();
    else { try { app.warm(); } catch (const std::exception& e) { error = e.what(); } }
    // Editor state. Times are epoch milliseconds; the view is the newest visible
    // edge and its length, and following keeps that edge at now.
    std::optional<Thumbnails> thumbs_holder; thumbs_holder.emplace(device); auto& thumbs = *thumbs_holder;
    std::optional<Waveforms> waves_holder; waves_holder.emplace(); auto& waves = *waves_holder;
    struct ScanResult { BufferMap map; bool indexed; };
    BufferMap map; std::future<ScanResult> scanning; bool indexed_map = false;
    std::vector<std::pair<std::int64_t, std::int64_t>> runs; bool map_has_mic = false;
    std::vector<std::int64_t> suffix_start;
    fs::path scanned_root; fs::file_time_type index_stamp{}; bool have_index_stamp = false; std::int64_t last_scan = 0, last_pin = 0;
    // The view has a target (what the user asked for) and a displayed value
    // that eases toward it, so pans and zooms glide instead of jumping.
    std::int64_t target_end_ms = now_ms(), in_ms = 0, out_ms = 0;
    double target_seconds = 240, shown_seconds = 240, shown_end_ms = (double)target_end_ms; bool animating = false;
    bool follow = true, open_settings = false;
    // Status lives on the action that started the work: the recorder's message
    // is watched for transitions, and a result shows on its button for a while.
    std::string last_message, notice, dismissed_failure; std::int64_t exported_at = 0, saved_at = 0, notice_at = 0; bool hotkey_warning_dismissed = false;
    constexpr std::int64_t ResultMs = 6000; bool line_open = false; // Whether the strip's error/notice row is showing.
    // Hover preview state persists across frames so it can fade and hold its last picture.
    Thumbnails::Picture hover_picture{}; float hover_alpha = 0, hover_x = 0; std::int64_t hover_shown_ms = 0;
    enum class Drag { None, Press, Range, In, Out } drag = Drag::None; std::int64_t drag_anchor = 0, scrub_ms = 0; float press_x = 0; bool scrubbing = false;
    std::optional<Player> player_holder; player_holder.emplace(device, context); auto& player = *player_holder;
    bool right_clear = false;
    auto clear_range = [&] { in_ms = out_ms = 0; player.set_range(0, 0); };
    auto inspect = [&](std::int64_t ms) { player.pause(); player.seek(ms); };
    const double frame_ms = 1000.0 / 60;
    auto span_at = [&](std::int64_t t) -> const Span* {
        // Index order is by end time. Start times can overlap across sessions.
        auto it = std::upper_bound(map.spans.begin(), map.spans.end(), t, [](auto time, const Span& s) { return time < s.end_ms; });
        for (; it != map.spans.end(); ++it) {
            if (suffix_start[(size_t)(it - map.spans.begin())] > t) break;
            if (t >= it->start_ms) return &*it;
        }
        return nullptr;
    };
    // Snap to a source frame of the segment that contains the time, so the
    // export's cut lands exactly there.
    auto snap = [&](std::int64_t t) {
        auto* s = span_at(t); if (!s) return t;
        return s->start_ms + (std::int64_t)std::llround(std::llround((t - s->start_ms) / frame_ms) * frame_ms);
    };
    struct WaveColumn { float x, peak, rms; };
    struct WaveCache {
        std::int64_t start = 0, end = 0, retry = 0; double scale = 0; float x = 0, width = 0;
        std::uint64_t revision = ~std::uint64_t(0); std::vector<WaveColumn> columns;
    };
    WaveCache wave_cache[2];
    struct FrameTimer {
        HANDLE value = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        ~FrameTimer() { if (value) CloseHandle(value); }
    } frame_timer;
    const auto frame_period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(frame_ms / 1000));
    auto frame_deadline = std::chrono::steady_clock::now();
    // Opt-in capture stays in memory during rendering and writes once on exit.
    wchar_t perf_path[32768]{}; bool perf_enabled = GetEnvironmentVariableW(L"FRINKY_CLIP_UI_PERF", perf_path, 32768) > 0;
    struct PerfRow { double interval, cpu, present; std::int64_t picture; size_t thumbnails; };
    std::vector<PerfRow> perf_rows;
    auto cpu_ms = [] { FILETIME created{}, exited{}, kernel{}, user{}; GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
        return ((double)(((std::uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                (double)(((std::uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime)) / 10000.0; };
    auto previous_frame = std::chrono::steady_clock::now();
    bool done = false;
    while (!done) {
        const auto frame_started = std::chrono::steady_clock::now();
        double cpu_started = perf_enabled ? cpu_ms() : 0; std::int64_t picture_ms = 0;
        Thumbnails::Picture preview_owner;
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        // Native hide/quit/focus changes can bypass ImGui's field-deactivation frame.
        if (dirty && settings_error.empty() && (app.quitting() || !IsWindowVisible(window) || GetForegroundWindow() != window)) save_settings();
        if (app.tick()) {
            if (!app.updating()) break;
            try { updates.apply(update_resume_recording); break; }
            catch (const std::exception& e) { app.cancel_update(); error = std::string("Update could not start: ") + e.what(); }
        }
        updates.tick(cfg.auto_check_updates && !app.quitting());
        if (app.update_action) {
            const int action = app.update_action; app.update_action = 0;
            if (action == 1) updates.check();
            if (action == 2) updates.download();
            if (action == 3) request_update();
        }
        if (app.start_requested) { app.start_requested = false; start_recording(); }
        HWND recorder = app.recorder();
        // The recorder announces each rewrite; the short poll only covers a lost message.
        if (app.status_changed || now_ms() - last_read > 1000) { status = read_json(app_dir() / "status.json"); last_read = now_ms(); app.status_changed = false; }
        bool current = obs_data_get_int(status.get(), "pid") == app.recorder_pid();
        std::int64_t updated_ms = obs_data_get_int(status.get(), "updated_ms");
        bool fresh = current && now_ms() - updated_ms < 5000;
        bool busy = app.active() && current && obs_data_get_bool(status.get(), "busy");
        auto failure = app.active() && !current ? std::string() : std::string(obs_data_get_string(status.get(), "error"));
        app.observe(fresh ? obs_data_get_string(status.get(), "state") : "", busy, current ? updated_ms : 0, !failure.empty());
        bool recording = app.recording(), paused = app.paused();
        bool visible = IsWindowVisible(window) && !IsIconic(window);
        if (scanning.valid() && scanning.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto result = scanning.get(); map = std::move(result.map); indexed_map = result.indexed; player.set_spans(map.spans);
                runs.clear(); map_has_mic = false; std::vector<fs::path> paths; paths.reserve(map.spans.size());
                for (const auto& s : map.spans) {
                    paths.push_back(s.path); map_has_mic |= s.coarse.size() > 1;
                    if (!runs.empty() && std::llabs(s.start_ms - runs.back().second) <= 1) runs.back().second = std::max(runs.back().second, s.end_ms);
                    else runs.emplace_back(s.start_ms, s.end_ms);
                }
                suffix_start.resize(map.spans.size());
                for (size_t i = map.spans.size(); i-- > 0;) suffix_start[i] = i + 1 < map.spans.size() ? std::min(map.spans[i].start_ms, suffix_start[i + 1]) : map.spans[i].start_ms;
                waves.retain(paths); thumbs.retain(paths);
            } catch (...) { indexed_map = false; }
            // First footage seen: park the playhead just before the live edge.
            if (!player.position() && map.last_end_ms) player.seek(std::max(map.spans.front().start_ms, map.last_end_ms - 1000));
        }
        player.set_end(map.last_end_ms);
        player.set_range(in_ms, out_ms);
        if (visible && !scanning.valid() && (app.index_changed || now_ms() - last_scan > 2000)) {
            bool announced = app.index_changed; app.index_changed = false;
            last_scan = now_ms(); auto root = cfg.storage / "buffer";
            // The recorder's index is one small file; scanning sidecars is the
            // fallback when no recorder has published one for this folder.
            auto index = app_dir() / "segments.json";
            std::error_code stamp_error; auto stamp = fs::last_write_time(index, stamp_error);
            bool unchanged = indexed_map && !announced && !stamp_error && have_index_stamp && stamp == index_stamp && root == scanned_root;
            have_index_stamp = !stamp_error; index_stamp = stamp; scanned_root = root;
            if (!unchanged) scanning = std::async(std::launch::async, [root, index] {
                if (auto map = read_index(index)) if (map->spans.empty() || map->spans.front().path.parent_path().parent_path() == root) return ScanResult{std::move(*map), true};
                return ScanResult{scan_buffer(root), false};
            });
        }
        if (!visible) {
            if (last_pin && recorder) { PostMessageW(recorder, PinMessage, 0, 0); last_pin = 0; }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT); continue;
        }
        float current_dpi = GetDpiForWindow(window) / 96.f; if (current_dpi != dpi) { dpi = current_dpi; theme(dpi); }
        thumbs.tick();
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Frinky Clip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::BeginDisabled(app.quitting());
        auto& state = app.state(); auto& style = ImGui::GetStyle();
        auto* draw = ImGui::GetWindowDrawList();
        bool changed = false, commit = false;
        double buffered = obs_data_get_double(status.get(), "buffer_seconds"), used = obs_data_get_double(status.get(), "buffer_gb");
        double export_fraction = current ? obs_data_get_double(status.get(), "export_progress") : -1;
        std::int64_t now = now_ms();
        // The overview always spans the configured history so the filled part
        // shows how much of it exists yet.
        double capacity = std::max(cfg.retention_minutes * 60.0, 10.0);
        std::int64_t oldest = now - (std::int64_t)(capacity * 1000);
        target_seconds = std::clamp(target_seconds, 10.0, capacity);
        if (follow) target_end_ms = now;
        target_end_ms = std::clamp(target_end_ms, oldest + (std::int64_t)(target_seconds * 1000), now);
        {
            // Ease the displayed view toward the target. While following, the
            // target advances with the clock; once close it is tracked exactly
            // so the idle loop can slow down.
            double alpha = 1 - std::exp(-std::clamp((double)io.DeltaTime, 0.0, 0.1) / 0.08);
            shown_seconds += (target_seconds - shown_seconds) * alpha;
            shown_end_ms += ((double)target_end_ms - shown_end_ms) * alpha;
            if (std::abs(shown_seconds - target_seconds) < 1e-3 * target_seconds) shown_seconds = target_seconds;
            if (std::abs(shown_end_ms - (double)target_end_ms) < (follow ? 1500.0 : 1.0)) shown_end_ms = (double)target_end_ms;
            animating = shown_seconds != target_seconds || shown_end_ms != (double)target_end_ms;
        }
        double view_seconds = shown_seconds; std::int64_t view_end_ms = (std::int64_t)std::llround(shown_end_ms);
        std::int64_t view_start_ms = view_end_ms - (std::int64_t)(view_seconds * 1000);
        bool have_range = in_ms && out_ms && out_ms > in_ms;
        auto accent_u32 = ImGui::ColorConvertFloat4ToU32(accent), muted_u32 = ImGui::ColorConvertFloat4ToU32(muted);
        auto line_u32 = ImGui::ColorConvertFloat4ToU32(rgb(0x2b2f35)), track_u32 = ImGui::ColorConvertFloat4ToU32(rgb(0x131518));
        auto footage_u32 = ImGui::ColorConvertFloat4ToU32(rgb(0x1e2227)), range_u32 = ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, .18f));

        // Editor header: buffer extent on the left, settings on the right.
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Buffer");
        ImGui::SameLine();
        if (map.spans.empty()) ImGui::TextDisabled("empty");
        else ImGui::TextDisabled("%s to now", local_time(map.spans.front().start_ms).c_str());
        if (!follow) { ImGui::SameLine(); if (ImGui::Button("Now")) follow = true; help("Follow latest footage"); }
        float settings_w = ImGui::CalcTextSize("Settings").x + style.FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - settings_w);
        if (ImGui::Button("Settings")) open_settings = true;

        // Overview bar: full history, newest at the right, footage, range, and the viewed window.
        {
            ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = 14 * dpi;
            auto ox = [&](std::int64_t t) { return p.x + w * (float)std::clamp((double)(t - oldest) / (capacity * 1000), 0.0, 1.0); };
            draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), track_u32);
            for (auto& run : runs) draw->AddRectFilled(ImVec2(ox(run.first), p.y), ImVec2(std::max(ox(run.second), ox(run.first) + 1), p.y + h), footage_u32);
            if (have_range) draw->AddRectFilled(ImVec2(ox(in_ms), p.y), ImVec2(std::max(ox(out_ms), ox(in_ms) + 2 * dpi), p.y + h), accent_u32);
            float x0 = ox(view_start_ms), x1 = std::max(ox(view_end_ms), x0 + 2 * dpi);
            draw->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + h), range_u32);
            draw->AddRect(ImVec2(x0, p.y), ImVec2(x1, p.y + h), accent_u32);
            ImGui::InvisibleButton("overview", ImVec2(w, h));
            if (ImGui::IsItemActive()) {
                std::int64_t at = oldest + (std::int64_t)(((io.MousePos.x - p.x) / w) * capacity * 1000);
                target_end_ms = at + (std::int64_t)(target_seconds * 500); follow = target_end_ms >= now - 500;
            }
        }

        // Lanes: wall-clock ruler, video with thumbnails, and audio coverage for the viewed window.
        float row_h = ImGui::GetFrameHeightWithSpacing();
        // Two clip rows, then the recorder strip: a padded action row, plus a
        // second row only while an error or notice is open.
        float below_h = 2 * dpi + style.ItemSpacing.y + row_h * 2 + 6 * dpi + style.ItemSpacing.y + style.WindowPadding.y + ImGui::GetFrameHeight() + style.ItemSpacing.y
            + (line_open ? ImGui::GetFrameHeightWithSpacing() : 0);
        // Fixed lane heights; whatever is left above them previews the cut frames.
        float audio_h = 40 * dpi, video_h = 96 * dpi, ruler_h = ImGui::GetTextLineHeight() + 4 * dpi;
        // The microphone lane appears when it is being recorded or any footage carries one.
        bool mic_lane = cfg.mic || map_has_mic;
        float lanes_h = ruler_h + 2 * dpi + video_h + (audio_h + 4 * dpi) * (mic_lane ? 2 : 1) + 4 * dpi + style.WindowPadding.y;
        float preview_h = std::max(96 * dpi, ImGui::GetContentRegionAvail().y - lanes_h - below_h - style.ItemSpacing.y);
        std::int64_t hover_ms = 0; bool hover_lane = false, hover_video = false, fading = false; auto tick_ms = steady_ms();
        // Viewport: the frame at the playhead, streaming while playing.
        std::int64_t playhead = player.position();
        if (ImGui::BeginChild("preview", ImVec2(0, preview_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
            float img_h = h, img_w = std::floor(img_h * 16 / 9);
            if (img_w > w) { img_w = w; img_h = std::floor(img_w * 9 / 16); }
            player.set_width((int)std::min(img_w * 2, 1920.f)); // Decode at up to 2x for crisp scaling.
            player.set_gains(cfg.desktop_gain / 100.f, cfg.mic_gain / 100.f);
            auto picture = player.tick();
            // While scrubbing or seeking, the keyframe at the target shows at
            // once (from the cache when it has been seen); the exact frame
            // replaces it when the decoder lands there.
            bool trimming = (drag == Drag::In || drag == Drag::Out) && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            std::int64_t preview_ms = trimming ? (drag == Drag::In ? in_ms : out_ms - 1)
                : scrubbing ? scrub_ms : (player.busy() && !player.playing()) ? player.position() : 0;
            // Frames the decoder presents on its way to the target are kept;
            // the keyframe only fills in until the first of them arrives.
            bool on_the_way = !trimming && !scrubbing && picture.texture && picture.ms <= preview_ms && picture.ms + 2500 >= preview_ms;
            if (preview_ms && !on_the_way) {
                if (auto* s = span_at(preview_ms)) {
                    auto key = trimming ? thumbs.frame(s->path, preview_ms - s->start_ms, (int)img_w)
                        : thumbs.keyframe(s->path, preview_ms - s->start_ms, (int)img_w);
                    if (trimming) picture = {}; // Never label the playhead's picture as the cut being inspected.
                    if (key.texture) { preview_owner = key; picture = {key.texture, key.width, key.height, preview_ms}; }
                } else if (trimming) picture = {};
            }
            picture_ms = picture.ms;
            float x = p.x + (w - img_w) / 2, y = p.y + (h - img_h) / 2;
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + img_w, y + img_h), track_u32);
            if (picture.texture) {
                float ph = std::min(img_h, img_w * picture.height / std::max(1, picture.width));
                draw->AddImage(picture.texture, ImVec2(x, y + (img_h - ph) / 2), ImVec2(x + img_w, y + (img_h - ph) / 2 + ph));
                draw->AddRect(ImVec2(x, y), ImVec2(x + img_w, y + img_h), IM_COL32(255, 255, 255, 26)); // Picture outline: a hairline of white at 10%.
            } else {
                auto problem = player.error();
                const char* hint = !problem.empty() ? problem.c_str()
                    : map.spans.empty() ? (recording ? "No footage yet. The first segment closes in a few seconds." : "No footage yet. Press Record to start the buffer.")
                    : player.busy() || trimming ? "Decoding…" : "Click the timeline to place the playhead. Space plays.";
                auto size = ImGui::CalcTextSize(hint); draw->AddText(ImVec2(p.x + (w - size.x) / 2, p.y + (h - size.y) / 2), muted_u32, hint);
            }
            // Playhead time in the corner of the picture.
            if (picture.texture) {
                auto stamp = trimming ? (drag == Drag::In ? "In " + local_time(in_ms, true) : "Before Out " + local_time(out_ms, true)) : local_time(picture.ms, true);
                auto size = ImGui::CalcTextSize(stamp.c_str());
                draw->AddRectFilled(ImVec2(x + 6 * dpi, y + img_h - size.y - 10 * dpi), ImVec2(x + size.x + 14 * dpi, y + img_h - 4 * dpi), IM_COL32(11, 12, 14, 200));
                draw->AddText(ImVec2(x + 10 * dpi, y + img_h - size.y - 7 * dpi), ImGui::ColorConvertFloat4ToU32(foreground), stamp.c_str());
            }
            ImGui::SetCursorScreenPos(ImVec2(x, y)); ImGui::InvisibleButton("viewport", ImVec2(std::max(img_w, 1.f), std::max(img_h, 1.f)));
            if (ImGui::IsItemClicked()) player.toggle();
            help(player.playing() ? "Pause (Space)" : have_range ? "Play clip (Space)" : "Play (Space)");
        }
        ImGui::EndChild();
        if (ImGui::BeginChild("lanes", ImVec2(0, lanes_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImVec2 origin = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, label_w = 44 * dpi;
            float track_x = origin.x + label_w, track_w = w - label_w;
            double px_per_ms = track_w / (view_seconds * 1000);
            auto x_of = [&](std::int64_t t) { return track_x + (float)((t - view_start_ms) * px_per_ms); };
            auto t_of = [&](float x) { return view_start_ms + (std::int64_t)((x - track_x) / px_per_ms); };
            float y = origin.y + ruler_h + 2 * dpi;
            // Lanes: video, desktop audio, and the microphone when footage has one.
            struct Lane { const char* name; float height; int track; };
            std::vector<Lane> lanes{{"video", video_h, -1}, {"audio", audio_h, 0}}; if (mic_lane) lanes.push_back({"mic", audio_h, 1});
            float bottom = y + video_h + (audio_h + 4 * dpi) * (mic_lane ? 2 : 1), video_y = y;
            const double steps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
            double step = steps[0]; for (double s : steps) { step = s; if (track_w * s / view_seconds >= 84 * dpi) break; }
            std::int64_t step_ms = (std::int64_t)(step * 1000);
            for (std::int64_t t = (view_start_ms / step_ms + 1) * step_ms; t <= view_end_ms; t += step_ms) {
                float x = x_of(t); auto label = local_time(t); auto size = ImGui::CalcTextSize(label.c_str());
                float tx = std::clamp(x - size.x / 2, track_x, track_x + track_w - size.x);
                draw->AddText(ImVec2(tx, origin.y), muted_u32, label.c_str());
                draw->AddLine(ImVec2(x, origin.y + ruler_h), ImVec2(x, bottom), line_u32);
            }
            // Ruler: click or drag scrubs the playhead; the seek lands on release.
            ImGui::SetCursorScreenPos(ImVec2(track_x, origin.y)); ImGui::InvisibleButton("ruler", ImVec2(track_w, ruler_h));
            if (ImGui::IsItemActive()) { player.pause(); scrub_ms = snap(std::clamp(t_of(io.MousePos.x), oldest, now)); scrubbing = true; }
            else if (scrubbing) { scrubbing = false; inspect(scrub_ms); }
            for (auto& lane : lanes) {
                draw->AddText(ImVec2(origin.x, y + (lane.height - ImGui::GetTextLineHeight()) / 2), muted_u32, lane.name);
                draw->AddRectFilled(ImVec2(track_x, y), ImVec2(track_x + track_w, y + lane.height), track_u32);
                bool audio_lane = lane.track >= 0;
                for (auto& run : runs) {
                    if (run.second < view_start_ms || run.first > view_end_ms) continue;
                    float x0 = std::max(x_of(run.first), track_x), x1 = std::min(x_of(run.second), track_x + track_w);
                    draw->AddRectFilled(ImVec2(x0, y + (audio_lane ? lane.height * .5f - 1 * dpi : 0)), ImVec2(x1, y + (audio_lane ? lane.height * .5f + 1 * dpi : lane.height)), footage_u32);
                }
                if (audio_lane) {
                    // Loudness waveform: for every pixel column, the loudest
                    // bin under it, drawn symmetrically about the lane's middle.
                    // Drawn the way editors draw a peak file: each column is
                    // the peak over the audio under it as a light envelope,
                    // with the RMS as a solid core inside. Two levels of the
                    // chain: the coarse one travels with the segment index and
                    // draws the whole buffer with no loads; the fine one is
                    // read from the sidecar when the view is close enough to
                    // need it and interpolated between bin centres. Peak is
                    // taken by max, so the zoomed-out silhouette is stable
                    // without any smoothing.
                    float mid = y + lane.height / 2, half = lane.height / 2 - 2 * dpi;
                    ImVec4 wave_colour = rgb(0x6b7688);
                    auto core_u32 = ImGui::ColorConvertFloat4ToU32(wave_colour), peak_u32 = ImGui::ColorConvertFloat4ToU32(ImVec4(wave_colour.x, wave_colour.y, wave_colour.z, .4f));
                    double ms_per_column = 1.0 / px_per_ms;
                    bool fine = ms_per_column < 4 * AudioBinMs;
                    auto height = [&](float v) { return v <= 0 ? 0.f : std::max(1.f, half * std::pow(v / 255.f, .7f)); }; // Mild curve keeps quiet passages visible.
                    auto& cached = wave_cache[lane.track];
                    auto revision = waves.revision();
                    if (cached.start != view_start_ms || cached.end != view_end_ms || cached.scale != px_per_ms ||
                        cached.x != track_x || cached.width != track_w || cached.revision != revision || steady_ms() - cached.retry > 5000) {
                        cached.start = view_start_ms; cached.end = view_end_ms; cached.scale = px_per_ms;
                        cached.x = track_x; cached.width = track_w; cached.revision = revision; cached.retry = steady_ms(); cached.columns.clear();
                        auto first = std::lower_bound(map.spans.begin(), map.spans.end(), view_start_ms, [](const Span& s, auto t) { return s.end_ms < t; });
                        for (auto it = first; it != map.spans.end(); ++it) {
                            if (suffix_start[(size_t)(it - map.spans.begin())] > view_end_ms) break;
                            const auto& s = *it; if (s.start_ms > view_end_ms) continue;
                            const AudioLevels* lv = nullptr; size_t track = (size_t)lane.track;
                            if (fine) if (auto* wave = waves.levels(s.path); wave && wave->size() > track && !(*wave)[track].empty()) lv = &(*wave)[track];
                            if (!lv) { if (s.coarse.size() <= track || s.coarse[track].empty()) continue; lv = &s.coarse[track]; }
                            double bin = lv->bin_ms; size_t count = lv->peak.size();
                            float xa = std::floor(std::max(x_of(s.start_ms), track_x)), xb = std::min(x_of(s.end_ms), track_x + track_w);
                            for (float x = xa; x < xb; x += 1) {
                                std::int64_t t0 = std::max<std::int64_t>(t_of(x), s.start_ms), t1 = std::min<std::int64_t>(t_of(x + 1), s.end_ms);
                                float peak = 0, rms = 0;
                                if (t1 - t0 >= bin) {
                                    size_t b0 = (size_t)((t0 - s.start_ms) / bin), b1 = std::min(count, (size_t)((t1 - s.start_ms) / bin) + 1);
                                    double squares = 0; for (size_t b = b0; b < b1; ++b) { peak = std::max(peak, (float)lv->peak[b]); double r = lv->rms[b]; squares += r * r; }
                                    rms = b1 > b0 ? (float)std::sqrt(squares / (b1 - b0)) : 0;
                                } else {
                                    double p = ((t0 + t1) / 2.0 - s.start_ms) / bin - 0.5; std::int64_t i = (std::int64_t)std::floor(p); float f = (float)(p - i);
                                    auto at = [&](const std::vector<std::uint8_t>& v, std::int64_t k) { return (float)v[(size_t)std::clamp<std::int64_t>(k, 0, (std::int64_t)count - 1)]; };
                                    peak = at(lv->peak, i) * (1 - f) + at(lv->peak, i + 1) * f; rms = at(lv->rms, i) * (1 - f) + at(lv->rms, i + 1) * f;
                                }
                                cached.columns.push_back({x + .5f, peak, rms});
                            }
                        }
                    }
                    for (const auto& column : cached.columns) {
                        float hp = height(column.peak), hr = height(column.rms), cx = column.x;
                        if (hp > 0) draw->AddLine(ImVec2(cx, mid - hp), ImVec2(cx, mid + hp), peak_u32, 1);
                        if (hr > 0) draw->AddLine(ImVec2(cx, mid - hr), ImVec2(cx, mid + hr), core_u32, 1);
                    }
                }
                draw->AddRect(ImVec2(track_x, y), ImVec2(track_x + track_w, y + lane.height), line_u32);
                y += lane.height + 4 * dpi;
            }
            // Filmstrip: thumbnails tile each run of footage edge to edge, one
            // keyframe per tile, so the lane is full at every zoom level.
            // Tiles align to absolute time, not to the run's start, so expiry
            // of the oldest footage does not move every tile.
            if (!runs.empty()) {
                float thumb_h = video_h - 2 * dpi, thumb_w = std::floor(thumb_h * 16 / 9);
                // The tile grid comes from the target zoom on a nested ladder
                // of spacings, so it is fixed for the whole glide and levels
                // share tile times: zooming in inserts tiles between existing
                // ones, zooming out drops every other one, and nothing shifts.
                // Within a level a tile's picture fills its time interval,
                // scaling with the displayed zoom and cropping the excess.
                double target_px_per_ms = track_w / (target_seconds * 1000);
                std::int64_t tile_ms = 500; double closest = 1e9;
                for (std::int64_t candidate = 500; candidate <= 3600000; candidate *= 2) {
                    double misfit = std::abs(std::log(candidate * target_px_per_ms / thumb_w));
                    if (misfit < closest) { closest = misfit; tile_ms = candidate; }
                }
                // Tiles draw up to 1.41x their natural width within a level;
                // decoding wider keeps them crisp. Decodes nearest the focus
                // (the cursor over the lanes, else the playhead) finish first.
                int decode_w = (int)(thumb_w * 3 / 2);
                bool mouse_in = io.MousePos.x >= track_x && io.MousePos.x <= track_x + track_w && io.MousePos.y >= origin.y && io.MousePos.y <= bottom;
                float focus_x = mouse_in ? io.MousePos.x : (playhead >= view_start_ms && playhead <= view_end_ms) ? x_of(playhead) : track_x + track_w / 2;
                draw->PushClipRect(ImVec2(track_x, video_y), ImVec2(track_x + track_w, video_y + video_h), true);
                for (auto& run : runs) {
                    if (run.second < view_start_ms || run.first > view_end_ms) continue;
                    // A tile whose keyframe is not decoded yet shows the last
                    // picture drawn in this run, so nothing blanks while the
                    // worker catches up; its own picture then fades in.
                    Thumbnails::Picture carry{};
                    auto tile = [&](std::int64_t t0, std::int64_t t1) {
                        auto* s = span_at(t0); if (!s) return;
                        float x0 = x_of(t0), full_w = x_of(t1) - x0, x1 = std::min(x0 + full_w, x_of(run.second));
                        auto own = thumbs.keyframe(s->path, t0 - s->start_ms, decode_w, (int)std::abs(x0 + full_w / 2 - focus_x));
                        auto picture = own.texture ? own : carry;
                        if (!picture.texture || full_w < 1 || x1 - x0 < 1) { if (own.texture) carry = own; return; }
                        // Fill the interval at the picture's aspect, cropping the excess about the centre.
                        float aspect = (float)picture.width / std::max(1, picture.height), natural_h = full_w / aspect;
                        ImVec2 uv0(0, 0), uv1(1, 1);
                        if (natural_h >= thumb_h) { float keep = thumb_h / natural_h; uv0.y = (1 - keep) / 2; uv1.y = 1 - uv0.y; }
                        else { float keep = full_w / (thumb_h * aspect); uv0.x = (1 - keep) / 2; uv1.x = 1 - uv0.x; }
                        uv1.x = uv0.x + (uv1.x - uv0.x) * (x1 - x0) / full_w; // Truncated at the run's end.
                        ImVec2 p0(x0, video_y + 1 * dpi), p1(x1, video_y + 1 * dpi + thumb_h);
                        float alpha = own.texture ? std::clamp((tick_ms - own.ready_ms) / 75.f, 0.f, 1.f) : 1.f;
                        if (alpha < 1) { fading = true; if (carry.texture && carry.texture != own.texture) draw->AddImage(carry.texture, p0, p1, uv0, uv1); }
                        draw->AddImage(picture.texture, p0, p1, uv0, uv1, IM_COL32(255, 255, 255, (int)(alpha * 255)));
                        if (own.texture) carry = own;
                    };
                    std::int64_t first = std::max(run.first, view_start_ms) / tile_ms * tile_ms;
                    if (first < run.first) { tile(run.first, first + tile_ms); first += tile_ms; } // The run's leading partial tile.
                    for (std::int64_t t = first; t < run.second && t <= view_end_ms; t += tile_ms) tile(t, t + tile_ms);
                }
                draw->PopClipRect();
                // Idle prefetch: one view width beyond each edge at this level
                // and the midpoints of the next level in, so the pan or zoom
                // that follows is a cache hit. Queued behind visible work.
                if (!animating && !scrubbing && drag == Drag::None && !player.playing()) {
                    int budget = 48; std::int64_t span_ms = view_end_ms - view_start_ms;
                    auto ask = [&](std::int64_t t, int priority) {
                        if (budget <= 0) return; auto* s = span_at(t); if (!s) return;
                        thumbs.prefetch(s->path, t - s->start_ms, decode_w, 1000000 + priority); --budget;
                    };
                    for (std::int64_t t = view_end_ms / tile_ms * tile_ms + tile_ms; t < view_end_ms + span_ms; t += tile_ms) ask(t, (int)((t - view_end_ms) / tile_ms));
                    for (std::int64_t t = view_start_ms / tile_ms * tile_ms; t > view_start_ms - span_ms; t -= tile_ms) ask(t, (int)((view_start_ms - t) / tile_ms));
                    if (tile_ms > 500) for (std::int64_t t = view_start_ms / tile_ms * tile_ms + tile_ms / 2; t < view_end_ms; t += tile_ms) ask(t, 100000 + (int)std::abs(x_of(t) - focus_x));
                }
            }
            // Live edge and the marked range.
            if (map.last_end_ms && map.last_end_ms >= view_start_ms && map.last_end_ms <= view_end_ms)
                draw->AddRectFilled(ImVec2(x_of(map.last_end_ms), video_y), ImVec2(std::min(x_of(now), track_x + track_w), video_y + video_h), ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, .08f)));
            if (follow) draw->AddLine(ImVec2(track_x + track_w - 1, origin.y + ruler_h), ImVec2(track_x + track_w - 1, bottom), accent_u32, 2 * dpi);
            if (have_range) {
                float x0 = x_of(in_ms), x1 = x_of(out_ms);
                draw->AddRectFilled(ImVec2(std::max(x0, track_x), origin.y + ruler_h), ImVec2(std::min(x1, track_x + track_w), bottom), range_u32);
                for (float x : {x0, x1}) if (x >= track_x && x <= track_x + track_w) draw->AddLine(ImVec2(x, origin.y + ruler_h), ImVec2(x, bottom), accent_u32, 2 * dpi);
            }
            // Playhead: a bright line with a marker in the ruler.
            {
                std::int64_t shown = scrubbing ? scrub_ms : playhead;
                if (shown >= view_start_ms && shown <= view_end_ms) {
                    float x = x_of(shown); auto head_u32 = ImGui::ColorConvertFloat4ToU32(foreground);
                    draw->AddLine(ImVec2(x, origin.y + ruler_h - 4 * dpi), ImVec2(x, bottom), head_u32, 1.5f * dpi);
                    draw->AddTriangleFilled(ImVec2(x - 5 * dpi, origin.y + ruler_h - 8 * dpi), ImVec2(x + 5 * dpi, origin.y + ruler_h - 8 * dpi), ImVec2(x, origin.y + ruler_h - 1 * dpi), head_u32);
                }
            }
            // Lane surface: left-drag marks or adjusts the range; wheel zooms; right-drag pans.
            ImGui::SetCursorScreenPos(ImVec2(track_x, origin.y + ruler_h));
            ImGui::InvisibleButton("lane-surface", ImVec2(track_w, std::max(bottom - origin.y - ruler_h, 1.f)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            bool hovered = ImGui::IsItemHovered();
            if (hovered) { hover_lane = true; hover_ms = t_of(io.MousePos.x); }
            // Hover preview: the keyframe under the cursor, shown large above
            // the strip with its time, for scanning footage without seeking.
            // The preview keeps its last picture and place until a new one is
            // ready, fades in fast and out slow, and never drops out for a
            // frame because a decode is still on the way.
            hover_video = hovered && io.MousePos.y >= video_y && io.MousePos.y <= video_y + video_h && drag == Drag::None && !scrubbing && !ImGui::IsAnyMouseDown();
            bool over_footage = false;
            if (hover_video && !runs.empty()) {
                if (auto* s = span_at(hover_ms)) {
                    over_footage = true;
                    float thumb_h = video_h - 2 * dpi; int decode_w = (int)(std::floor(thumb_h * 16 / 9) * 3 / 2);
                    auto picture = thumbs.keyframe(s->path, hover_ms - s->start_ms, decode_w, INT_MIN + 1);
                    if (picture.texture) { hover_picture = picture; hover_shown_ms = hover_ms; }
                    hover_x = io.MousePos.x;
                }
            }
            {
                // Only footage under the cursor holds the preview; a gap lets it
                // fade, so a quick pass across one keeps it and resting on one does not.
                float goal = over_footage && hover_picture.texture ? 1.f : 0.f;
                double tau = goal > hover_alpha ? 0.03 : 0.125;
                hover_alpha += (goal - hover_alpha) * (float)(1 - std::exp(-std::clamp((double)io.DeltaTime, 0.0, 0.1) / tau));
                if (std::abs(hover_alpha - goal) < 0.02f) hover_alpha = goal;
                if (hover_alpha != goal) fading = true;
                if (hover_alpha == 0) hover_picture = {}; // Never hold a texture the cache may drop.
            }
            if (hover_alpha > 0 && hover_picture.texture) {
                float a = hover_alpha; auto tint = [&](ImU32 colour, float scale) { return (colour & 0x00ffffff) | ((ImU32)(((colour >> 24) & 255) * a * scale) << 24); };
                float x = std::clamp(hover_x, track_x, track_x + track_w);
                draw->AddLine(ImVec2(x, video_y), ImVec2(x, video_y + video_h), tint(accent_u32, 1), 1 * dpi);
                float pw = (float)hover_picture.width, ph = pw * hover_picture.height / std::max(1, hover_picture.width), pad = 4 * dpi, text_h = ImGui::GetTextLineHeight();
                float px = std::clamp(x - pw / 2, track_x, std::max(track_x, track_x + track_w - pw));
                float py = std::max(pad, origin.y - ph - text_h - pad * 3 - 6 * dpi);
                auto* fg = ImGui::GetForegroundDrawList();
                fg->AddRectFilled(ImVec2(px - pad, py - pad), ImVec2(px + pw + pad, py + ph + text_h + pad * 2), tint(IM_COL32(11, 12, 14, 235), 1));
                fg->AddRect(ImVec2(px - pad, py - pad), ImVec2(px + pw + pad, py + ph + text_h + pad * 2), tint(line_u32, 1));
                fg->AddImage(hover_picture.texture, ImVec2(px, py), ImVec2(px + pw, py + ph), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(a * 255)));
                fg->AddText(ImVec2(px, py + ph + pad), tint(muted_u32, 1), local_time(hover_shown_ms, true).c_str());
            }
            float grab = 6 * dpi;
            if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
                right_clear = (in_ms && std::abs(io.MousePos.x - x_of(in_ms)) <= grab) ||
                    (out_ms && std::abs(io.MousePos.x - x_of(out_ms)) <= grab);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) right_clear = false;
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                if (right_clear && hovered) { clear_range(); have_range = false; }
                right_clear = false;
            }
            // A press becomes a range drag once the mouse moves; a plain click
            // places the playhead instead.
            if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                float mx = io.MousePos.x; press_x = mx;
                if (have_range && std::abs(mx - x_of(in_ms)) <= grab) drag = Drag::In;
                else if (have_range && std::abs(mx - x_of(out_ms)) <= grab) drag = Drag::Out;
                else { drag = Drag::Press; drag_anchor = snap(t_of(mx)); }
                player.pause();
            }
            if (ImGui::IsItemActive() && drag != Drag::None) {
                std::int64_t t = snap(std::clamp(t_of(io.MousePos.x), oldest, now));
                if (drag == Drag::Press && std::abs(io.MousePos.x - press_x) > 4 * dpi) { drag = Drag::Range; in_ms = out_ms = 0; }
                if (drag == Drag::Range) { in_ms = std::min(drag_anchor, t); out_ms = std::max(drag_anchor, t); }
                if (drag == Drag::In) { in_ms = std::min(t, out_ms - (std::int64_t)frame_ms); }
                if (drag == Drag::Out) { out_ms = std::max(t, in_ms + (std::int64_t)frame_ms); }
            } else if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right) && !right_clear && io.MouseDelta.x != 0) {
                target_end_ms -= (std::int64_t)(io.MouseDelta.x / px_per_ms); follow = false;
            }
            if (!ImGui::IsItemActive()) {
                if (drag == Drag::Press) inspect(drag_anchor);
                if (drag == Drag::Range && in_ms == out_ms) in_ms = out_ms = 0;
                drag = Drag::None;
            }
            if (hovered && io.MouseWheel != 0) {
                // Zoom about the cursor in target space, so the moment under
                // the mouse is still there once the glide settles.
                double fraction = std::clamp((io.MousePos.x - track_x) / track_w, 0.f, 1.f);
                std::int64_t anchor = target_end_ms - (std::int64_t)(target_seconds * 1000 * (1 - fraction));
                double next = std::clamp(target_seconds * std::pow(1.25, -io.MouseWheel), 10.0, capacity);
                target_end_ms = anchor + (std::int64_t)((target_end_ms - anchor) * next / target_seconds);
                target_seconds = next; follow = target_end_ms >= now - 500;
            }
            if (hovered && (drag == Drag::In || drag == Drag::Out || drag == Drag::Range)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            else if (hovered && have_range && (std::abs(io.MousePos.x - x_of(in_ms)) <= grab || std::abs(io.MousePos.x - x_of(out_ms)) <= grab)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        ImGui::EndChild();
        // Keyboard transport when no field has focus: Space plays, I/O mark at
        // the playhead, arrows step a frame (a second with Shift).
        if (!io.WantTextInput && !ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) player.toggle();
            if (ImGui::IsKeyPressed(ImGuiKey_I, false)) { in_ms = snap(playhead); if (!out_ms || out_ms <= in_ms) out_ms = 0; }
            if (ImGui::IsKeyPressed(ImGuiKey_O, false)) { out_ms = snap(playhead); if (!in_ms || in_ms >= out_ms) in_ms = 0; }
            std::int64_t stride = io.KeyShift ? 1000 : (std::int64_t)std::llround(frame_ms);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) inspect(std::max(oldest, playhead - stride));
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) inspect(std::min(now, playhead + stride));
        }
        player.set_range(in_ms, out_ms);
        have_range = in_ms && out_ms && out_ms > in_ms;
        // Keep a playing playhead on screen by paging the view forward.
        if (player.playing() && !follow && playhead > view_end_ms) { target_end_ms = playhead + (std::int64_t)(target_seconds * 900); }

        // Clip rows: unboxed, directly under the lanes they describe. Both
        // share the content's leading edge (Play) and trailing edge (Export).
        ImGui::Dummy(ImVec2(0, 2 * dpi));
        ImGui::AlignTextToFramePadding();
        if (ImGui::Button(player.playing() ? "Pause" : "Play", ImVec2(60 * dpi, 0))) player.toggle();
        help(player.playing() ? "Pause (Space)" : have_range ? "Play clip (Space)" : "Play (Space)");
        ImGui::SameLine(); ImGui::TextUnformatted(playhead ? local_time(playhead, true).c_str() : "--:--:--.---");
        ImGui::SameLine(0, 16 * dpi);
        if (have_range) {
            double seconds = (out_ms - in_ms) / 1000.0;
            ImGui::TextDisabled("In"); ImGui::SameLine(); ImGui::TextUnformatted(local_time(in_ms, true).c_str());
            ImGui::SameLine(0, 10 * dpi); ImGui::TextDisabled("Out"); ImGui::SameLine(); ImGui::TextUnformatted(local_time(out_ms, true).c_str());
            ImGui::SameLine(0, 10 * dpi); ImGui::TextDisabled("%.3f s (%lld frames)", seconds, (long long)std::llround(seconds * 60));
            ImGui::SameLine(0, 10 * dpi); if (ImGui::Button("Clear")) { clear_range(); have_range = false; }
        } else {
            ImGui::TextDisabled("%s", in_ms ? ("In " + local_time(in_ms, true) + "   press O to mark the out point").c_str()
                : out_ms ? ("Out " + local_time(out_ms, true) + "   press I to mark the in point").c_str() : "Drag the lane or press I and O to mark a range");
        }
        float control_w = ImGui::GetFontSize() * 5.5f;
        ImGui::SetNextItemWidth(control_w); int res = cfg.export_height == 720 ? 0 : cfg.export_height == 1440 ? 2 : 1;
        if (ImGui::Combo("##res", &res, "720p\0" "1080p\0" "1440p\0")) { cfg.export_height = res == 0 ? 720 : res == 2 ? 1440 : 1080; changed = commit = true; }
        help("Export resolution");
        ImGui::SameLine(); ImGui::SetNextItemWidth(control_w); int fps = cfg.export_fps == 30 ? 0 : 1;
        if (ImGui::Combo("##fps", &fps, "30 fps\0" "60 fps\0")) { cfg.export_fps = fps == 0 ? 30 : 60; changed = commit = true; }
        ImGui::SameLine(); ImGui::SetNextItemWidth(control_w); int codec = cfg.export_codec == "av1" ? 1 : 0;
        if (ImGui::Combo("##codec", &codec, "H.264\0" "AV1\0")) { cfg.export_codec = codec ? "av1" : "h264"; changed = commit = true; }
        help("Export codec");
        ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
        { float mbps = cfg.share_bitrate / 1000.f; bool edited = ImGui::InputFloat("##export-bitrate", &mbps, 0, 0, "%.1f"); commit |= ImGui::IsItemDeactivatedAfterEdit();
          if (edited && std::isfinite(mbps) && mbps >= 0 && mbps <= 1000) { cfg.share_bitrate = (int)std::round(mbps * 1000); changed = true; } }
        if (reset_to(cfg.share_bitrate, defaults.share_bitrate)) changed = commit = true;
        help("Export bitrate");
        ImGui::SameLine(0, style.ItemInnerSpacing.x); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Mbps");
        auto sources = have_range ? spans_in_range(map.spans, in_ms, out_ms) : std::vector<Span>{};
        // The microphone option follows the footage: it is offered when every
        // segment in the marked range carries a microphone track.
        bool mic_available = !sources.empty() && std::all_of(sources.begin(), sources.end(), [](const Span& s) { return s.coarse.size() > 1; });
        // Mix levels, shared by playback and export so what you hear is what
        // you get. 0% mutes a track; the microphone slider is live whenever
        // there is a microphone track to level.
        ImGui::SameLine(0, 14 * dpi); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Desktop"); ImGui::SameLine(0, style.ItemInnerSpacing.x);
        ImGui::SetNextItemWidth(64 * dpi); if (ImGui::SliderInt("##desktop-gain", &cfg.desktop_gain, 0, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp)) changed = true; commit |= ImGui::IsItemDeactivatedAfterEdit();
        if (reset_to(cfg.desktop_gain, defaults.desktop_gain)) changed = commit = true;
        help("Playback and export volume");
        ImGui::SameLine(0, 10 * dpi); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Mic"); ImGui::SameLine(0, style.ItemInnerSpacing.x);
        ImGui::BeginDisabled(!mic_lane); ImGui::SetNextItemWidth(64 * dpi); if (ImGui::SliderInt("##mic-gain", &cfg.mic_gain, 0, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp)) changed = true; commit |= ImGui::IsItemDeactivatedAfterEdit(); ImGui::EndDisabled();
        if (reset_to(cfg.mic_gain, defaults.mic_gain)) changed = commit = true;
        help(mic_lane ? "Playback and export volume" : "No microphone track");
        bool closed = have_range && !sources.empty() && out_ms <= map.last_end_ms + 1;
        bool can_export = closed && recorder && !busy && !app.quitting();
        // Watch the recorder's message for results and notices.
        auto message = std::string(obs_data_get_string(status.get(), "message"));
        if (message != last_message) {
            if (message.starts_with("Exported ")) exported_at = now;
            else if (message.starts_with("Saved ")) saved_at = now;
            else if (!message.empty() && message != "Recording" && message != "Paused" && !message.starts_with("Saving") && !message.starts_with("Exporting") &&
                     !message.starts_with("Starting") && !message.starts_with("Stopping") && !message.starts_with("Quitting")) { notice = message; notice_at = now; }
            last_message = message;
        }
        bool exporting = busy && export_fraction >= 0, exported = now - exported_at < ResultMs;
        // The button is the status: it fills with progress, then reads the result.
        float export_w = ImGui::CalcTextSize("Exported").x + style.FramePadding.x * 2 + 12 * dpi;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - export_w);
        ImGui::BeginDisabled(!can_export || exporting);
        std::string export_label = exporting ? std::to_string((int)std::lround(export_fraction * 100)) + "%" : exported ? "Exported" : "Export";
        bool pressed = exporting || exported ? ImGui::Button(export_label.c_str(), ImVec2(export_w, 0)) : primary_button(export_label.c_str(), ImVec2(export_w, 0));
        if (exporting) {
            ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
            draw->AddRectFilled(r0, ImVec2(r0.x + (r1.x - r0.x) * (float)export_fraction, r1.y), ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, .7f)), style.FrameRounding);
        }
        if (pressed && !exporting) {
            try {
                ExportRequest request; request.start_ms = in_ms; request.end_ms = out_ms; request.height = cfg.export_height; request.fps = cfg.export_fps;
                request.bitrate_kbps = cfg.share_bitrate; request.codec = cfg.export_codec; request.audio = cfg.desktop_gain > 0; request.mic = mic_available && cfg.mic_gain > 0;
                request.desktop_gain = cfg.desktop_gain / 100.0; request.mic_gain = cfg.mic_gain / 100.0;
                write_export_request(app_dir() / "export-request.json", request);
                PostMessageW(recorder, ExportMessage, 0, 0); error.clear();
            } catch (const std::exception& e) { error = e.what(); }
        }
        ImGui::EndDisabled();
        if (exported) help(message.c_str());
        else if (!exporting) {
            if (!have_range) help("No range selected");
            else if (!closed) help(sources.empty() ? "Range must contain footage from a single recording session" : "Selected footage is still buffering");
            else if (busy) help("Save or export in progress");
        }
        // Keep the viewed and marked footage from expiring while the editor shows it.
        if (recorder && now - last_pin > 2000) {
            std::int64_t a = have_range ? std::min(in_ms, view_start_ms) : view_start_ms, b = have_range ? std::max(out_ms, view_end_ms) : view_end_ms;
            PostMessageW(recorder, PinMessage, (WPARAM)a, (LPARAM)b); last_pin = now;
        }

        // Recorder strip: a filled surface running edge to edge along the
        // bottom of the window, a different surface for a different subject.
        // State and buffer on the leading side, actions on the trailing side,
        // one message line beneath. Its height never changes.
        {
            float pad = style.WindowPadding.y, frame_h = ImGui::GetFrameHeight(); // The window's bottom padding is the strip's bottom inset; the top matches it.
            ImGui::Dummy(ImVec2(0, 6 * dpi));
            ImVec2 top = ImGui::GetCursorScreenPos(), win = ImGui::GetWindowPos(), win_size = ImGui::GetWindowSize();
            draw->AddRectFilled(ImVec2(win.x, top.y), ImVec2(win.x + win_size.x, win.y + win_size.y), track_u32);
            draw->AddLine(ImVec2(win.x, top.y), ImVec2(win.x + win_size.x, top.y), line_u32);
            ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + pad));
            auto dot = ImGui::GetCursorScreenPos();
            draw->AddCircleFilled(ImVec2(dot.x + 4 * dpi, dot.y + frame_h / 2), 3.5f * dpi, recording ? ImGui::ColorConvertFloat4ToU32(foreground) : muted_u32);
            ImGui::Dummy(ImVec2(12 * dpi, frame_h)); ImGui::SameLine(); ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(state == "quitting" ? "Quitting…" : state == "stopping" ? "Stopping…" : recording ? "Recording" : state == "starting" ? "Starting…" : "Paused");
            ImGui::SameLine(0, 10 * dpi); ImGui::PushFont(regular, 19); ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(duration(buffered).c_str()); ImGui::PopFont();
            help("Buffered duration");
            char summary[64]; snprintf(summary, sizeof(summary), "%.1f / %.0f GB", used, cfg.budget_gb);
            ImGui::SameLine(0, 14 * dpi); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", summary);
            // Disk gauge.
            ImGui::SameLine(0, 8 * dpi);
            {
                ImVec2 g = ImGui::GetCursorScreenPos(); float gw = 64 * dpi, gh = 5 * dpi, gy = g.y + (frame_h - gh) / 2;
                float fill = (float)std::clamp(used / std::max(.1, cfg.budget_gb), 0., 1.);
                draw->AddRectFilled(ImVec2(g.x, gy), ImVec2(g.x + gw, gy + gh), line_u32, 1 * dpi);
                draw->AddRectFilled(ImVec2(g.x, gy), ImVec2(g.x + gw * fill, gy + gh), ImGui::ColorConvertFloat4ToU32(rgb(0x5b6470)), 1 * dpi);
                ImGui::Dummy(ImVec2(gw, frame_h));
                help("Buffer disk usage");
            }
            // Save shows its own progress and result, like Export.
            bool saving = busy && message.starts_with("Saving"), saved = now - saved_at < ResultMs;
            auto save_label = std::string(saving ? "Saving…" : saved ? "Saved" : "Save last " + std::to_string(cfg.save_seconds) + " s"); auto keys = shortcut(cfg);
            float button_w = 90 * dpi, save_w = ImGui::CalcTextSize(("Save last " + std::to_string(cfg.save_seconds) + " s").c_str()).x + style.FramePadding.x * 2;
            float folder_w = ImGui::CalcTextSize("Folder").x + style.FramePadding.x * 2, keys_w = ImGui::CalcTextSize(keys.c_str()).x;
            float cluster_w = button_w + style.ItemSpacing.x + save_w + style.ItemInnerSpacing.x + keys_w + 16 * dpi + folder_w;
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - cluster_w);
            if (paused) {
                if (ImGui::Button("Record", ImVec2(button_w, 0))) start_recording();
            } else {
                ImGui::BeginDisabled(!recording); if (ImGui::Button("Stop", ImVec2(button_w, 0))) app.stop(); ImGui::EndDisabled();
            }
            ImGui::SameLine(); ImGui::BeginDisabled(!recording || busy);
            if (ImGui::Button(save_label.c_str(), ImVec2(save_w, 0))) PostMessageW(recorder, SaveMessage, cfg.save_seconds, 0);
            if (saved) help(message.c_str());
            ImGui::EndDisabled();
            ImGui::SameLine(0, style.ItemInnerSpacing.x); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", keys.c_str());
            ImGui::SameLine(0, 16 * dpi); if (ImGui::Button("Folder")) open_path(window, cfg.storage / "clips", error, true);
            help("Open clips folder");
            // Errors and notices are rare, so they earn a line of their own:
            // the strip grows by one row while one exists and shrinks back
            // when it is dismissed or, for a notice, after a few seconds.
            if (error.empty() && !app.error.empty()) error = app.error;
            bool hotkey_warning = recording && !obs_data_get_bool(status.get(), "hotkey_registered") && !hotkey_warning_dismissed;
            bool notice_live = !notice.empty() && now - notice_at < ResultMs;
            std::string line; bool is_error = true; int source = 0;
            if (!error.empty()) { line = error; source = 1; }
            else if (!failure.empty() && failure != dismissed_failure) { line = failure; source = 2; }
            else if (!settings_error.empty()) { line = "Not saved: " + settings_error; source = 3; }
            else if (hotkey_warning) { line = "Save hotkey unavailable. Stop and choose another shortcut."; source = 4; }
            else if (notice_live) { line = notice; is_error = false; source = 5; }
            line_open = !line.empty();
            if (line_open) {
                float dismiss_w = ImGui::CalcTextSize("Dismiss").x + style.FramePadding.x * 2;
                ImVec2 at = ImGui::GetCursorScreenPos(); float line_w = ImGui::GetContentRegionAvail().x - dismiss_w - style.ItemSpacing.x;
                ImVec4 clip(at.x, at.y, at.x + line_w, at.y + frame_h);
                draw->AddText(nullptr, 0, ImVec2(at.x, at.y + style.FramePadding.y), is_error ? ImGui::ColorConvertFloat4ToU32(rgb(0xf0a399)) : ImGui::ColorConvertFloat4ToU32(foreground), line.c_str(), nullptr, 0, &clip);
                ImGui::Dummy(ImVec2(line_w, frame_h));
                if (ImGui::CalcTextSize(line.c_str()).x > line_w) help(line.c_str());
                ImGui::SameLine();
                if (ImGui::Button("Dismiss", ImVec2(dismiss_w, 0))) {
                    if (source == 1) error.clear(); else if (source == 2) dismissed_failure = failure; else if (source == 3) settings_error.clear();
                    else if (source == 4) hotkey_warning_dismissed = true; else notice.clear();
                }
            }
        }

        // Settings overlay. Capture fields stay locked while a session is active.
        if (open_settings) { ImGui::OpenPopup("Settings"); open_settings = false; }
        ImGui::SetNextWindowSize(ImVec2(std::min(600 * dpi, io.DisplaySize.x - 40 * dpi), 0));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(.5f, .5f));
        bool settings_open = true;
        if (ImGui::BeginPopupModal("Settings", &settings_open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
            ImGui::BeginDisabled(!paused); section(paused ? "Capture" : "Capture (stop recording to edit)");
            if (properties("capture")) {
                row("Display"); std::string display_label = displays.empty() ? "No monitor" : displays.front().label;
                for (auto& d : displays) if (d.id == cfg.monitor) display_label = d.label;
                if (ImGui::BeginCombo("##display", display_label.c_str())) { for (auto& d : displays) if (ImGui::Selectable(d.label.c_str(), d.id == cfg.monitor)) { cfg.monitor = d.id; changed = commit = true; } ImGui::EndCombo(); }
                row("Audio"); if (ImGui::Checkbox("Desktop", &cfg.audio)) changed = commit = true; help("Audio from the default playback device");
                ImGui::SameLine(0, 12 * dpi); if (ImGui::Checkbox("Microphone", &cfg.mic)) changed = commit = true;
                // The device row is always present, so turning the microphone on does not reflow the dialog.
                row("Microphone"); std::string mic_label = "Default microphone";
                for (auto& d : microphones) if (d.id == cfg.mic_device) mic_label = d.label;
                ImGui::BeginDisabled(!cfg.mic);
                if (ImGui::BeginCombo("##microphone", mic_label.c_str())) { for (auto& d : microphones) if (ImGui::Selectable(d.label.c_str(), d.id == cfg.mic_device)) { cfg.mic_device = d.id; changed = commit = true; } ImGui::EndCombo(); }
                ImGui::EndDisabled();
                changed |= bitrate_row("Bitrate", cfg.bitrate, commit, defaults.bitrate);
                changed |= bitrate_row("Max bitrate", cfg.max_bitrate, commit, defaults.max_bitrate);
                ImGui::EndTable();
            }
            section("Buffer & hotkey");
            if (properties("retention")) {
                changed |= int_row("History", "##history", cfg.retention_minutes, "min", commit, defaults.retention_minutes);
                row("Disk budget"); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f); changed |= ImGui::InputDouble("##budget", &cfg.budget_gb, 0, 0, "%.1f"); commit |= ImGui::IsItemDeactivatedAfterEdit();
                if (reset_to(cfg.budget_gb, defaults.budget_gb)) changed = commit = true;
                help("Excludes saved clips"); ImGui::SameLine(); ImGui::TextDisabled("GB");
                changed |= int_row("Save duration", "##seconds", cfg.save_seconds, "sec", commit, defaults.save_seconds);
                row("Save hotkey"); ImGui::SetNextItemWidth(64 * dpi); int key = (int)cfg.hotkey - VK_F1;
                if (ImGui::Combo("##hotkey", &key, "F1\0F2\0F3\0F4\0F5\0F6\0F7\0F8\0F9\0F10\0F11\0F12\0")) { cfg.hotkey = VK_F1 + key; changed = commit = true; }
                ImGui::SameLine(); bool ctrl = cfg.modifiers & MOD_CONTROL, shift = cfg.modifiers & MOD_SHIFT, alt = cfg.modifiers & MOD_ALT;
                if (ImGui::Checkbox("Ctrl", &ctrl)) changed = commit = true; ImGui::SameLine(); if (ImGui::Checkbox("Shift", &shift)) changed = commit = true; ImGui::SameLine(); if (ImGui::Checkbox("Alt", &alt)) changed = commit = true;
                cfg.modifiers = (ctrl ? MOD_CONTROL : 0) | (shift ? MOD_SHIFT : 0) | (alt ? MOD_ALT : 0);
                row("Storage"); changed |= ImGui::InputText("##storage", storage, sizeof(storage)); commit |= ImGui::IsItemDeactivatedAfterEdit(); help(storage); ImGui::EndTable();
            }
            ImGui::EndDisabled();
            section("App");
            if (properties("app")) {
                row("Startup");
                bool windows_startup = starts_with_windows();
                ImGui::BeginDisabled(!updates.status().installed);
                if (ImGui::Checkbox("Start with Windows", &windows_startup)) {
                    try { set_starts_with_windows(windows_startup); error.clear(); }
                    catch (const std::exception& e) { error = e.what(); }
                }
                ImGui::EndDisabled();
                help(updates.status().installed ? "Open in the tray when you sign in" : "Install Frinky Clip to enable Windows startup");
                row("");
                if (ImGui::Checkbox("Record on launch", &cfg.record_on_launch)) {
                    try {
                        // Persist this independent preference without applying draft capture settings.
                        auto saved = Config::load(); saved.record_on_launch = cfg.record_on_launch; saved.save();
                        error.clear();
                    } catch (const std::exception& e) { cfg.record_on_launch = !cfg.record_on_launch; error = e.what(); }
                }
                ImGui::EndTable();
            }
            section("Updates");
            auto update = updates.status();
            if (properties("updates")) {
                row("Version"); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(FRINKY_VERSION);
                row("Updates");
                ImGui::BeginDisabled(!update.installed);
                if (ImGui::Checkbox("Check automatically", &cfg.auto_check_updates)) {
                    try {
                        auto saved = Config::load(); saved.auto_check_updates = cfg.auto_check_updates; saved.save();
                    } catch (const std::exception& e) { cfg.auto_check_updates = !cfg.auto_check_updates; error = e.what(); }
                }
                ImGui::EndDisabled();
                row("");
                const char* action = app.updating() ? "Restarting..." : update.working ? (update.available ? "Downloading..." : "Checking...")
                    : update.ready ? "Restart" : update.available ? "Download" : "Check";
                ImGui::BeginDisabled(!update.installed || update.working || app.quitting() || (update.ready && !app.paused() && !app.recording()));
                // Keep the same item identity and width through every update state.
                if (ImGui::Button((std::string(action) + "###update-action").c_str(), ImVec2(152 * dpi, 0))) {
                    if (update.ready) request_update();
                    else if (update.available) updates.download();
                    else updates.check();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Release notes")) open_path(window,
                    fs::path(wide("https://github.com/frinky04/frinky-clip/releases/tag/v" + (update.version.empty() ? std::string(FRINKY_VERSION) : update.version))), error);
                ImGui::EndTable();
            }
            std::string update_status = !update.installed ? "Install Frinky Clip to enable updates."
                : app.updating() ? "Finishing saves and exports..."
                : !update.error.empty() ? (update.available ? "Download failed. Try again." : "Could not check for updates. Try again.")
                : update.working ? (update.available ? "Downloading " + std::to_string(update.progress) + "%" : "Checking for updates...")
                : update.ready ? "Version " + update.version + " ready to install"
                : update.available ? "Version " + update.version + " available"
                : update.message.empty() ? "" : "You're up to date";
            // One reserved line, including progress. Errors never resize the dialog.
            ImVec2 update_at = ImGui::GetCursorScreenPos();
            float update_w = ImGui::GetContentRegionAvail().x, frame_h = ImGui::GetFrameHeight();
            ImVec4 update_clip(update_at.x, update_at.y, update_at.x + update_w, update_at.y + frame_h);
            ImGui::GetWindowDrawList()->AddText(nullptr, 0, ImVec2(update_at.x, update_at.y + style.FramePadding.y),
                update.error.empty() ? muted_u32 : ImGui::ColorConvertFloat4ToU32(rgb(0xf0a399)), update_status.c_str(), nullptr, 0, &update_clip);
            if (update.working && update.available)
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(update_at.x, update_at.y + frame_h - 2 * dpi),
                    ImVec2(update_at.x + update_w * std::clamp(update.progress / 100.f, 0.f, 1.f), update_at.y + frame_h), accent_u32);
            ImGui::Dummy(ImVec2(update_w, frame_h));
            if (!update.error.empty()) help(update.error.c_str());
            section("Diagnostics");
            if (ImGui::Button("Open log")) open_path(window, app_dir() / "recorder.log", error);
            if (recording) {
                ImGui::SameLine(); ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%.1f fps | %.2f ms render", obs_data_get_double(status.get(), "fps"), obs_data_get_double(status.get(), "render_ms"));
            }
            ImGui::TextDisabled("Missed frames: %lld render / %lld encode", obs_data_get_int(status.get(), "lagged_frames"), obs_data_get_int(status.get(), "skipped_frames"));
            help("Totals for the last reported session");
            ImGui::TextDisabled("Closed segments: %zu | thumbnails: %llu decoded, %zu cached, last %.0f ms | player decode: %s", map.spans.size(),
                (unsigned long long)thumbs.decodes(), thumbs.cached(), thumbs.last_decode_ms(), player.hardware() ? "D3D11VA" : "software");
            if (!settings_error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, rgb(0xf0a399));
                ImGui::TextWrapped("Not saved: %s", settings_error.c_str()); ImGui::PopStyleColor();
            }
            ImGui::Spacing();
            float close_w = ImGui::CalcTextSize("Close").x + style.FramePadding.x * 2;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - close_w);
            if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (changed) { dirty = true; settings_error.clear(); }
        if (dirty && settings_error.empty() && (commit || !ImGui::IsAnyItemActive())) save_settings();
        ImGui::EndDisabled();
        bool active_input = ImGui::IsAnyItemActive() || io.WantTextInput || thumbs.busy() || drag != Drag::None || player.busy() || scrubbing || animating || fading || hover_video;
        ImGui::End(); ImGui::Render(); const float clear[4] = {background.x, background.y, background.z, 1};
        auto present_started = std::chrono::steady_clock::now();
        HRESULT presented = S_OK;
        if (target) { context->OMSetRenderTargets(1, &target, nullptr); context->ClearRenderTargetView(target, clear); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); present_started = std::chrono::steady_clock::now(); presented = swapchain->Present(1, 0); }
        if (perf_enabled) {
            auto ended = std::chrono::steady_clock::now();
            perf_rows.push_back({std::chrono::duration<double, std::milli>(frame_started - previous_frame).count(),
                cpu_ms() - cpu_started, std::chrono::duration<double, std::milli>(ended - present_started).count(), picture_ms, thumbs.cached()});
            previous_frame = frame_started;
        }
        // Input wakes immediately; idle controls need no fast render loop.
        if (presented == DXGI_STATUS_OCCLUDED || !active_input || !target) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, presented == DXGI_STATUS_OCCLUDED ? 250 : 100, QS_ALLINPUT);
        } else {
            // Reanchor after idle or an early input wake; otherwise carry the
            // deadline forward so scheduler overshoot does not accumulate.
            if (frame_started < frame_deadline || frame_started - frame_deadline > frame_period) frame_deadline = frame_started;
            frame_deadline += frame_period;
            double remaining = std::chrono::duration<double, std::milli>(frame_deadline - std::chrono::steady_clock::now()).count();
            if (remaining > 0) {
                LARGE_INTEGER due; due.QuadPart = -std::max<LONGLONG>(1, (LONGLONG)std::llround(remaining * 10000));
                if (frame_timer.value && SetWaitableTimer(frame_timer.value, &due, 0, nullptr, nullptr, FALSE))
                    MsgWaitForMultipleObjects(1, &frame_timer.value, FALSE, INFINITE, QS_ALLINPUT);
                else MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)remaining, QS_ALLINPUT);
            }
        }
    }
    if (perf_enabled) {
        std::ofstream output{fs::path(perf_path)};
        output << "frame_interval_ms,ui_cpu_ms,present_ms,picture_ms,thumbnails_cached\n";
        for (const auto& row : perf_rows) output << row.interval << ',' << row.cpu << ',' << row.present << ',' << row.picture << ',' << row.thumbnails << '\n';
    }
    if (scanning.valid()) scanning.wait();
    if (auto worker = app.recorder()) PostMessageW(worker, PinMessage, 0, 0);
    player_holder.reset(); thumbs_holder.reset(); waves_holder.reset();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (target) { target->Release(); target = nullptr; } swapchain->Release(); context->Release(); device->Release();
    device = nullptr; context = nullptr; swapchain = nullptr; DestroyWindow(window); UnregisterClassW(AppWindowClass, wc.hInstance); return 0;
}
}
