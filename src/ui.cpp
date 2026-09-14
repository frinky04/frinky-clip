#include "recorder.hpp"
#include "app.hpp"
#include "timeline.hpp"
#include "thumbs.hpp"
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
constexpr int DefaultClientWidth = 760, MinClientWidth = 720, ChromeHeight = 332, MinPreviewHeight = 96, SidePadding = 14;
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
bool bitrate_row(const char* name, int& kbps, bool& commit) {
    row(name); ImGui::PushID(name); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
    float mbps = kbps / 1000.f; bool changed = ImGui::InputFloat("##value", &mbps, 0, 0, "%.1f");
    commit |= ImGui::IsItemDeactivatedAfterEdit();
    if (changed && std::isfinite(mbps) && mbps >= 0 && mbps <= 1000) kbps = (int)std::round(mbps * 1000);
    ImGui::SameLine(); ImGui::TextDisabled("Mbps"); ImGui::PopID(); return changed;
}
bool int_row(const char* label, const char* id, int& value, const char* unit, bool& commit) {
    row(label); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
    bool changed = ImGui::InputInt(id, &value, 0, 0); commit |= ImGui::IsItemDeactivatedAfterEdit(); ImGui::SameLine(); ImGui::TextDisabled("%s", unit); return changed;
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
int run_ui() {
    App app;
    if (!app.primary()) {
        if (auto existing = FindWindowW(AppWindowClass, nullptr)) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        return 0;
    }
    Config cfg = Config::load(); auto displays = monitors();
    char storage[2048]; strncpy_s(storage, path_text(cfg.storage).c_str(), _TRUNCATE);
    auto status = read_json(app_dir() / "status.json"); std::int64_t last_read = 0;
    std::string error, settings_error; bool dirty = false;
    WNDCLASSW wc{}; wc.style = CS_CLASSDC; wc.lpfnWndProc = ui_proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = AppWindowClass; RegisterClassW(&wc);
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
    ShowWindow(window, SW_SHOW); SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW); UpdateWindow(window);
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
    // The recorder process is launched once and stays warm; without auto-record
    // it idles with the OBS core initialized so the first Record is quick.
    if (cfg.record_on_launch) start_recording();
    else { try { app.warm(); } catch (const std::exception& e) { error = e.what(); } }
    // Editor state. Times are epoch milliseconds; the view is the newest visible
    // edge and its length, and following keeps that edge at now.
    std::optional<Thumbnails> thumbs_holder; thumbs_holder.emplace(device); auto& thumbs = *thumbs_holder;
    BufferMap map; std::future<BufferMap> scanning; std::int64_t last_scan = 0, last_pin = 0;
    // The view has a target (what the user asked for) and a displayed value
    // that eases toward it, so pans and zooms glide instead of jumping.
    std::int64_t target_end_ms = now_ms(), in_ms = 0, out_ms = 0;
    double target_seconds = 240, shown_seconds = 240, shown_end_ms = (double)target_end_ms; bool animating = false;
    bool follow = true, open_settings = false, export_system = true;
    // Status lives on the action that started the work: the recorder's message
    // is watched for transitions, and a result shows on its button for a while.
    std::string last_message, notice, dismissed_failure; std::int64_t exported_at = 0, saved_at = 0, notice_at = 0; bool hotkey_warning_dismissed = false;
    constexpr std::int64_t ResultMs = 6000; bool line_open = false; // Whether the strip's error/notice row is showing.
    // Hover preview state persists across frames so it can fade and hold its last picture.
    Thumbnails::Picture hover_picture{}; float hover_alpha = 0, hover_x = 0; std::int64_t hover_shown_ms = 0;
    enum class Drag { None, Press, Range, In, Out } drag = Drag::None; std::int64_t drag_anchor = 0, scrub_ms = 0; float press_x = 0; bool scrubbing = false;
    std::optional<Player> player_holder; player_holder.emplace(device, context); auto& player = *player_holder;
    const double frame_ms = 1000.0 / 60;
    auto span_at = [&](std::int64_t t) -> const Span* { for (auto& s : map.spans) if (t >= s.start_ms && t < s.end_ms) return &s; return nullptr; };
    // Snap to a source frame of the segment that contains the time, so the
    // export's cut lands exactly there.
    auto snap = [&](std::int64_t t) {
        auto* s = span_at(t); if (!s) return t;
        return s->start_ms + (std::int64_t)std::llround(std::llround((t - s->start_ms) / frame_ms) * frame_ms);
    };
    bool done = false;
    while (!done) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        // Native hide/quit/focus changes can bypass ImGui's field-deactivation frame.
        if (dirty && settings_error.empty() && (app.quitting() || !IsWindowVisible(window) || GetForegroundWindow() != window)) save_settings();
        if (app.tick()) break;
        if (app.start_requested) { app.start_requested = false; start_recording(); }
        HWND recorder = app.recorder();
        // The recorder announces each rewrite; the short poll only covers a lost message.
        if (app.status_changed || now_ms() - last_read > 100) { status = read_json(app_dir() / "status.json"); last_read = now_ms(); app.status_changed = false; }
        bool current = obs_data_get_int(status.get(), "pid") == app.recorder_pid();
        std::int64_t updated_ms = obs_data_get_int(status.get(), "updated_ms");
        bool fresh = current && now_ms() - updated_ms < 5000;
        bool busy = app.active() && current && obs_data_get_bool(status.get(), "busy");
        auto failure = app.active() && !current ? std::string() : std::string(obs_data_get_string(status.get(), "error"));
        app.observe(fresh ? obs_data_get_string(status.get(), "state") : "", busy, current ? updated_ms : 0, !failure.empty());
        bool recording = app.recording(), paused = app.paused();
        bool visible = IsWindowVisible(window) && !IsIconic(window);
        if (scanning.valid() && scanning.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try { map = scanning.get(); player.set_spans(map.spans); } catch (...) {}
            // First footage seen: park the playhead just before the live edge.
            if (!player.position() && map.last_end_ms) player.seek(std::max(map.spans.front().start_ms, map.last_end_ms - 1000));
        }
        player.set_end(map.last_end_ms);
        if (visible && !scanning.valid() && now_ms() - last_scan > 2000) {
            last_scan = now_ms(); auto root = cfg.storage / "buffer";
            // The recorder's index is one small file; scanning sidecars is the
            // fallback when no recorder has published one for this folder.
            auto index = app_dir() / "segments.json";
            scanning = std::async(std::launch::async, [root, index] {
                if (auto map = read_index(index)) if (map->spans.empty() || map->spans.front().path.parent_path().parent_path() == root) return *map;
                return scan_buffer(root);
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
        ImGui::Begin("Frinky Clip", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
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
        // Contiguous footage runs: segments of a session chain exactly, so a
        // run is a sequence of spans that meet end to start.
        std::vector<std::pair<std::int64_t, std::int64_t>> runs;
        for (auto& s : map.spans) {
            if (!runs.empty() && std::llabs(s.start_ms - runs.back().second) <= 1) runs.back().second = std::max(runs.back().second, s.end_ms);
            else runs.emplace_back(s.start_ms, s.end_ms);
        }
        auto accent_u32 = ImGui::ColorConvertFloat4ToU32(accent), muted_u32 = ImGui::ColorConvertFloat4ToU32(muted);
        auto line_u32 = ImGui::ColorConvertFloat4ToU32(rgb(0x2b2f35)), track_u32 = ImGui::ColorConvertFloat4ToU32(rgb(0x131518));
        auto footage_u32 = ImGui::ColorConvertFloat4ToU32(rgb(0x1e2227)), range_u32 = ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, .18f));

        // Editor header: buffer extent on the left, settings on the right.
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Buffer");
        ImGui::SameLine();
        if (map.spans.empty()) ImGui::TextDisabled("empty");
        else ImGui::TextDisabled("%s to now", local_time(map.spans.front().start_ms).c_str());
        if (!follow) { ImGui::SameLine(); if (ImGui::Button("Now")) follow = true; help("Return to the live edge."); }
        float settings_w = ImGui::CalcTextSize("Settings").x + style.FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - settings_w);
        if (ImGui::Button("Settings")) open_settings = true;
        help("Capture, buffer, hotkey, app, and diagnostics.");

        // Overview bar: full history, newest at the right, footage, range, and the viewed window.
        {
            ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = 14 * dpi;
            auto ox = [&](std::int64_t t) { return p.x + w * (float)std::clamp((double)(t - oldest) / (capacity * 1000), 0.0, 1.0); };
            draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), track_u32);
            for (auto& s : map.spans) draw->AddRectFilled(ImVec2(ox(s.start_ms), p.y), ImVec2(std::max(ox(s.end_ms), ox(s.start_ms) + 1), p.y + h), footage_u32);
            if (have_range) draw->AddRectFilled(ImVec2(ox(in_ms), p.y), ImVec2(std::max(ox(out_ms), ox(in_ms) + 2 * dpi), p.y + h), accent_u32);
            float x0 = ox(view_start_ms), x1 = std::max(ox(view_end_ms), x0 + 2 * dpi);
            draw->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1, p.y + h), range_u32);
            draw->AddRect(ImVec2(x0, p.y), ImVec2(x1, p.y + h), accent_u32);
            ImGui::InvisibleButton("overview", ImVec2(w, h));
            if (ImGui::IsItemActive()) {
                std::int64_t at = oldest + (std::int64_t)(((io.MousePos.x - p.x) / w) * capacity * 1000);
                target_end_ms = at + (std::int64_t)(target_seconds * 500); follow = target_end_ms >= now - 500;
            }
            help("Whole history. Drag to move the viewed window; scroll in the lanes to zoom.");
        }

        // Lanes: wall-clock ruler, video with thumbnails, and audio coverage for the viewed window.
        float row_h = ImGui::GetFrameHeightWithSpacing();
        // Two clip rows, then the recorder strip: a padded action row, plus a
        // second row only while an error or notice is open.
        float below_h = 2 * dpi + style.ItemSpacing.y + row_h * 2 + 6 * dpi + style.ItemSpacing.y + 8 * dpi * 2 + ImGui::GetFrameHeight() + style.WindowPadding.y
            + (line_open ? ImGui::GetFrameHeightWithSpacing() : 0);
        // Fixed lane heights; whatever is left above them previews the cut frames.
        float audio_h = 30 * dpi, video_h = 96 * dpi, ruler_h = ImGui::GetTextLineHeight() + 4 * dpi;
        float lanes_h = ruler_h + 2 * dpi + video_h + (audio_h + 4 * dpi) + 4 * dpi + style.WindowPadding.y;
        float preview_h = std::max(96 * dpi, ImGui::GetContentRegionAvail().y - lanes_h - below_h - style.ItemSpacing.y);
        std::int64_t hover_ms = 0; bool hover_lane = false, hover_video = false, fading = false; auto tick_ms = steady_ms();
        // Viewport: the frame at the playhead, streaming while playing.
        std::int64_t playhead = player.position();
        if (ImGui::BeginChild("preview", ImVec2(0, preview_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetContentRegionAvail().y;
            float img_h = h, img_w = std::floor(img_h * 16 / 9);
            if (img_w > w) { img_w = w; img_h = std::floor(img_w * 9 / 16); }
            player.set_width((int)std::min(img_w * 2, 1920.f)); // Decode at up to 2x for crisp scaling.
            auto picture = player.tick();
            // While scrubbing or seeking, the keyframe at the target shows at
            // once (from the cache when it has been seen); the exact frame
            // replaces it when the decoder lands there.
            std::int64_t preview_ms = scrubbing ? scrub_ms : (player.busy() && !player.playing()) ? player.position() : 0;
            // Frames the decoder presents on its way to the target are kept;
            // the keyframe only fills in until the first of them arrives.
            bool on_the_way = !scrubbing && picture.texture && picture.ms <= preview_ms && picture.ms + 2500 >= preview_ms;
            if (preview_ms && !on_the_way) {
                if (auto* s = span_at(preview_ms)) {
                    auto key = thumbs.keyframe(s->path, preview_ms - s->start_ms, (int)img_w);
                    if (key.texture) picture = {key.texture, key.width, key.height, preview_ms};
                }
            }
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
                    : player.busy() ? "Decoding…" : "Click the timeline to place the playhead. Space plays.";
                auto size = ImGui::CalcTextSize(hint); draw->AddText(ImVec2(p.x + (w - size.x) / 2, p.y + (h - size.y) / 2), muted_u32, hint);
            }
            // Playhead time in the corner of the picture.
            if (picture.texture) {
                auto stamp = local_time(picture.ms, true); auto size = ImGui::CalcTextSize(stamp.c_str());
                draw->AddRectFilled(ImVec2(x + 6 * dpi, y + img_h - size.y - 10 * dpi), ImVec2(x + size.x + 14 * dpi, y + img_h - 4 * dpi), IM_COL32(11, 12, 14, 200));
                draw->AddText(ImVec2(x + 10 * dpi, y + img_h - size.y - 7 * dpi), ImGui::ColorConvertFloat4ToU32(foreground), stamp.c_str());
            }
            ImGui::SetCursorScreenPos(ImVec2(x, y)); ImGui::InvisibleButton("viewport", ImVec2(std::max(img_w, 1.f), std::max(img_h, 1.f)));
            if (ImGui::IsItemClicked()) player.toggle();
            help("Click or press Space to play and pause.");
        }
        ImGui::EndChild();
        if (ImGui::BeginChild("lanes", ImVec2(0, lanes_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImVec2 origin = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, label_w = 44 * dpi;
            float track_x = origin.x + label_w, track_w = w - label_w;
            double px_per_ms = track_w / (view_seconds * 1000);
            auto x_of = [&](std::int64_t t) { return track_x + (float)((t - view_start_ms) * px_per_ms); };
            auto t_of = [&](float x) { return view_start_ms + (std::int64_t)((x - track_x) / px_per_ms); };
            float y = origin.y + ruler_h + 2 * dpi;
            // A microphone lane joins these once a microphone track is recorded.
            struct Lane { const char* name; float height; } lanes[] = {{"video", video_h}, {"audio", audio_h}};
            float bottom = y + video_h + (audio_h + 4 * dpi), video_y = y;
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
            if (ImGui::IsItemActive()) { scrub_ms = snap(std::clamp(t_of(io.MousePos.x), oldest, now)); scrubbing = true; }
            else if (scrubbing) { scrubbing = false; player.seek(scrub_ms); }
            help("Click or drag to move the playhead. Scroll to zoom, right-drag to pan.");
            for (auto& lane : lanes) {
                draw->AddText(ImVec2(origin.x, y + (lane.height - ImGui::GetTextLineHeight()) / 2), muted_u32, lane.name);
                draw->AddRectFilled(ImVec2(track_x, y), ImVec2(track_x + track_w, y + lane.height), track_u32);
                bool audio_lane = lane.name[0] != 'v';
                for (auto& run : runs) {
                    if (run.second < view_start_ms || run.first > view_end_ms) continue;
                    float x0 = std::max(x_of(run.first), track_x), x1 = std::min(x_of(run.second), track_x + track_w);
                    draw->AddRectFilled(ImVec2(x0, y + (audio_lane ? lane.height * .35f : 0)), ImVec2(x1, y + (audio_lane ? lane.height * .65f : lane.height)), footage_u32);
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
            // A press becomes a range drag once the mouse moves; a plain click
            // places the playhead instead.
            if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                float mx = io.MousePos.x; press_x = mx;
                if (have_range && std::abs(mx - x_of(in_ms)) <= grab) drag = Drag::In;
                else if (have_range && std::abs(mx - x_of(out_ms)) <= grab) drag = Drag::Out;
                else { drag = Drag::Press; drag_anchor = snap(t_of(mx)); }
            }
            if (ImGui::IsItemActive() && drag != Drag::None) {
                std::int64_t t = snap(std::clamp(t_of(io.MousePos.x), oldest, now));
                if (drag == Drag::Press && std::abs(io.MousePos.x - press_x) > 4 * dpi) { drag = Drag::Range; in_ms = out_ms = 0; }
                if (drag == Drag::Range) { in_ms = std::min(drag_anchor, t); out_ms = std::max(drag_anchor, t); }
                if (drag == Drag::In) { in_ms = std::min(t, out_ms - (std::int64_t)frame_ms); }
                if (drag == Drag::Out) { out_ms = std::max(t, in_ms + (std::int64_t)frame_ms); }
            } else if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right) && io.MouseDelta.x != 0) {
                target_end_ms -= (std::int64_t)(io.MouseDelta.x / px_per_ms); follow = false;
            }
            if (!ImGui::IsItemActive()) {
                if (drag == Drag::Press) player.seek(drag_anchor);
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
        if (hover_lane && !hover_video && !have_range) help("Click to place the playhead, drag to mark a range. Scroll to zoom, right-drag to pan.");
        // Keyboard transport when no field has focus: Space plays, I/O mark at
        // the playhead, arrows step a frame (a second with Shift).
        if (!io.WantTextInput && !ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) player.toggle();
            if (ImGui::IsKeyPressed(ImGuiKey_I, false)) { in_ms = snap(playhead); if (!out_ms || out_ms <= in_ms) out_ms = 0; }
            if (ImGui::IsKeyPressed(ImGuiKey_O, false)) { out_ms = snap(playhead); if (!in_ms || in_ms >= out_ms) in_ms = 0; }
            std::int64_t stride = io.KeyShift ? 1000 : (std::int64_t)std::llround(frame_ms);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) player.seek(std::max(oldest, playhead - stride));
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) player.seek(std::min(now, playhead + stride));
        }
        // Keep a playing playhead on screen by paging the view forward.
        if (player.playing() && !follow && playhead > view_end_ms) { target_end_ms = playhead + (std::int64_t)(target_seconds * 900); }

        // Clip rows: unboxed, directly under the lanes they describe. Both
        // share the content's leading edge (Play) and trailing edge (Export).
        ImGui::Dummy(ImVec2(0, 2 * dpi));
        ImGui::AlignTextToFramePadding();
        if (ImGui::Button(player.playing() ? "Pause" : "Play", ImVec2(60 * dpi, 0))) player.toggle();
        help("Space plays and pauses. Arrow keys step one frame; Shift+arrows step one second.");
        ImGui::SameLine(); ImGui::TextUnformatted(playhead ? local_time(playhead, true).c_str() : "--:--:--.---");
        ImGui::SameLine(0, 16 * dpi);
        if (have_range) {
            double seconds = (out_ms - in_ms) / 1000.0;
            ImGui::TextDisabled("In"); ImGui::SameLine(); ImGui::TextUnformatted(local_time(in_ms, true).c_str());
            ImGui::SameLine(0, 10 * dpi); ImGui::TextDisabled("Out"); ImGui::SameLine(); ImGui::TextUnformatted(local_time(out_ms, true).c_str());
            ImGui::SameLine(0, 10 * dpi); ImGui::TextDisabled("%.3f s (%lld frames)", seconds, (long long)std::llround(seconds * 60));
            ImGui::SameLine(0, 10 * dpi); if (ImGui::Button("Clear")) { in_ms = out_ms = 0; }
        } else {
            ImGui::TextDisabled("%s", in_ms ? ("In " + local_time(in_ms, true) + "   press O to mark the out point").c_str()
                : out_ms ? ("Out " + local_time(out_ms, true) + "   press I to mark the in point").c_str() : "Drag the lane or press I and O to mark a range");
        }
        float control_w = ImGui::GetFontSize() * 5.5f;
        ImGui::SetNextItemWidth(control_w); int res = cfg.export_height == 720 ? 0 : cfg.export_height == 1440 ? 2 : 1;
        if (ImGui::Combo("##res", &res, "720p\0" "1080p\0" "1440p\0")) { cfg.export_height = res == 0 ? 720 : res == 2 ? 1440 : 1080; changed = commit = true; }
        help("Export resolution. Frame-accurate re-encode with NVENC.");
        ImGui::SameLine(); ImGui::SetNextItemWidth(control_w); int fps = cfg.export_fps == 30 ? 0 : 1;
        if (ImGui::Combo("##fps", &fps, "30 fps\0" "60 fps\0")) { cfg.export_fps = fps == 0 ? 30 : 60; changed = commit = true; }
        ImGui::SameLine(); ImGui::SetNextItemWidth(control_w); int codec = cfg.export_codec == "av1" ? 1 : 0;
        if (ImGui::Combo("##codec", &codec, "H.264\0" "AV1\0")) { cfg.export_codec = codec ? "av1" : "h264"; changed = commit = true; }
        help("H.264 plays everywhere. AV1 is smaller at the same quality but needs newer players.");
        ImGui::SameLine(); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
        { float mbps = cfg.share_bitrate / 1000.f; bool edited = ImGui::InputFloat("##export-bitrate", &mbps, 0, 0, "%.1f"); commit |= ImGui::IsItemDeactivatedAfterEdit();
          if (edited && std::isfinite(mbps) && mbps >= 0 && mbps <= 1000) { cfg.share_bitrate = (int)std::round(mbps * 1000); changed = true; } }
        help("Export bitrate.");
        ImGui::SameLine(0, style.ItemInnerSpacing.x); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Mbps");
        ImGui::SameLine(0, 12 * dpi); ImGui::Checkbox("Desktop audio", &export_system); help("Include desktop audio in the export. A microphone track is planned.");
        auto sources = have_range ? spans_in_range(map.spans, in_ms, out_ms) : std::vector<Span>{};
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
                request.bitrate_kbps = cfg.share_bitrate; request.codec = cfg.export_codec; request.audio = export_system;
                write_export_request(app_dir() / "export-request.json", request);
                PostMessageW(recorder, ExportMessage, 0, 0); error.clear();
            } catch (const std::exception& e) { error = e.what(); }
        }
        ImGui::EndDisabled();
        help(exporting ? "Exporting the clip." : exported ? message.c_str() : !have_range ? "Mark a range first." : !closed ? (sources.empty() ? "The range crosses a Stop/Record boundary or has no footage." : "Wait for the last segment to close.")
            : busy ? "A save or export is in progress." : "Re-encode the range to the clips folder.");
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
            float pad = 8 * dpi, frame_h = ImGui::GetFrameHeight();
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
            help("Footage in the buffer. Oldest footage expires at the history or disk limit.");
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
                help("Disk budget used by the buffer. Saved clips are stored separately.");
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
            help(saved ? message.c_str() : "Quick clip of the most recent footage at recording quality. Saves whole segments, so it may include a few extra seconds."); ImGui::EndDisabled();
            ImGui::SameLine(0, style.ItemInnerSpacing.x); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", keys.c_str());
            ImGui::SameLine(0, 16 * dpi); if (ImGui::Button("Folder")) open_path(window, cfg.storage / "clips", error, true);
            help("Open the clips folder.");
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
                row("Audio"); if (ImGui::Checkbox("Desktop", &cfg.audio)) changed = commit = true; help("Records the default playback device. Microphone and per-app filtering are not enabled in this prototype.");
                changed |= bitrate_row("Bitrate", cfg.bitrate, commit); changed |= bitrate_row("Max bitrate", cfg.max_bitrate, commit);
                ImGui::EndTable();
            }
            section("Buffer & hotkey");
            if (properties("retention")) {
                changed |= int_row("History", "##history", cfg.retention_minutes, "min", commit);
                row("Disk budget"); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f); changed |= ImGui::InputDouble("##budget", &cfg.budget_gb, 0, 0, "%.1f"); commit |= ImGui::IsItemDeactivatedAfterEdit(); ImGui::SameLine(); ImGui::TextDisabled("GB");
                changed |= int_row("Save duration", "##seconds", cfg.save_seconds, "sec", commit);
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
                if (ImGui::Checkbox("Record on launch", &cfg.record_on_launch)) {
                    try {
                        // Persist this independent preference without applying draft capture settings.
                        auto saved = Config::load(); saved.record_on_launch = cfg.record_on_launch; saved.save();
                        error.clear();
                    } catch (const std::exception& e) { cfg.record_on_launch = !cfg.record_on_launch; error = e.what(); }
                }
                help("Saved immediately. Starts recording when the tray app starts, not when the controls reopen. Does not launch with Windows.");
                ImGui::EndTable();
            }
            section("Diagnostics");
            if (ImGui::Button("Open log")) open_path(window, app_dir() / "recorder.log", error);
            if (recording) {
                ImGui::SameLine(); ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%.1f fps | %.2f ms render", obs_data_get_double(status.get(), "fps"), obs_data_get_double(status.get(), "render_ms"));
            }
            ImGui::TextDisabled("Missed frames: %lld render / %lld encode", obs_data_get_int(status.get(), "lagged_frames"), obs_data_get_int(status.get(), "skipped_frames"));
            help("Cumulative recorder misses for the last reported session. These do not measure the game's FPS impact.");
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
        if (target) { context->OMSetRenderTargets(1, &target, nullptr); context->ClearRenderTargetView(target, clear); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); swapchain->Present(1, 0); }
        // Input wakes immediately; idle controls need no fast render loop.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, active_input ? 16 : 100, QS_ALLINPUT);
    }
    if (scanning.valid()) scanning.wait();
    if (auto worker = app.recorder()) PostMessageW(worker, PinMessage, 0, 0);
    player_holder.reset(); thumbs_holder.reset();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (target) { target->Release(); target = nullptr; } swapchain->Release(); context->Release(); device->Release();
    device = nullptr; context = nullptr; swapchain = nullptr; DestroyWindow(window); UnregisterClassW(AppWindowClass, wc.hInstance); return 0;
}
}
