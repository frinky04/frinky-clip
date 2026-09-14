#pragma once
#include <windows.h>
#include <obs.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace clip {
namespace fs = std::filesystem;
using Data = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>;
inline Data data() { return Data(obs_data_create(), obs_data_release); }
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
std::string path_text(const fs::path& path);
fs::path exe_dir();
fs::path app_dir();
std::int64_t now_ms();
std::string unique_id();
std::string read_text(const fs::path& path);
void atomic_write(const fs::path& path, const std::string& text);
bool flush_closed(const fs::path& path);
Data read_json(const fs::path& path);
void write_json(const fs::path& path, obs_data_t* value);
std::wstring quote_arg(const std::wstring& arg);
DWORD run_process(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& log, DWORD timeout_ms = 300000);
void launch(const std::vector<std::wstring>& args);

struct Config {
    int bitrate = 40000;
    int max_bitrate = 50000;
    int retention_minutes = 120;
    double budget_gb = 50;
    int save_seconds = 60;
    int share_bitrate = 20000;
    int export_height = 1080; // Clip editor export defaults: 720, 1080, or 1440 lines.
    int export_fps = 60;      // 30 or 60.
    std::string export_codec = "h264"; // or "av1"
    unsigned hotkey = VK_F8;
    unsigned modifiers = MOD_CONTROL | MOD_SHIFT;
    bool audio = true;
    bool record_on_launch = true;
    std::string monitor;
    fs::path storage;
    static Config load();
    void save() const;
    void validate() const;
};

constexpr wchar_t RecorderClass[] = L"FrinkyClip.Recorder.0.1";
constexpr UINT SaveMessage = WM_APP + 1;
constexpr UINT StopMessage = WM_APP + 2;
constexpr UINT ShareMessage = WM_APP + 3;
constexpr UINT TrayMessage = WM_APP + 4;
constexpr UINT ResumeMessage = WM_APP + 7; // Recorder: start a capture session in the warm process.
constexpr UINT ExitMessage = WM_APP + 8;   // Recorder: stop capture, finish saves, and exit.
constexpr UINT PinMessage = WM_APP + 9;    // Recorder: protect footage in [wParam, lParam] epoch ms from expiry; (0, 0) releases.
constexpr UINT ExportMessage = WM_APP + 10; // Recorder: export the clip described by export-request.json.
constexpr UINT StatusMessage = WM_APP + 11; // Controls: status.json was just rewritten; read it now.
constexpr UINT WakeMessage = WM_APP + 12;   // Recorder: an OBS callback fired; run the tick without waiting for the timer.
void plain_write(const fs::path& path, const std::string& text); // Atomic rename without the disk flush, for transient files.
void write_json_fast(const fs::path& path, obs_data_t* value);
HWND recorder_window();
struct Monitor { std::string id, label; int width, height; bool primary; };
std::vector<Monitor> monitors();
}
