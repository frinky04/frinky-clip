#include "common.hpp"
#include <shlobj.h>
#include <fstream>
#include <stdexcept>
#include <chrono>
#include <sstream>
#include <algorithm>

namespace clip {
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid Unicode text");
    std::string out(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr); return out;
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (!n) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring out(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n); return out;
}
std::string path_text(const fs::path& p) { return utf8(p.generic_wstring()); }
fs::path exe_dir() { wchar_t p[32768]; GetModuleFileNameW(nullptr, p, 32768); return fs::path(p).parent_path(); }
fs::path app_dir() {
    // Isolates integration tests from the user's real recording/configuration.
    wchar_t override_path[32768];
    if (GetEnvironmentVariableW(L"FRINKY_CLIP_HOME", override_path, 32768)) return fs::absolute(override_path);
    PWSTR p = nullptr; SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p);
    fs::path result = fs::path(p) / "FrinkyClip"; CoTaskMemFree(p); return result;
}
std::int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string unique_id() { GUID id; CoCreateGuid(&id); wchar_t s[40]; StringFromGUID2(id, s, 40); return std::to_string(now_ms()) + "-" + utf8(s).substr(1, 8); }
std::string read_text(const fs::path& p) { std::ifstream in(p, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
void atomic_write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    fs::path tmp = p; tmp += L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot write " + path_text(tmp));
    DWORD written = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot commit " + path_text(p));
}
bool flush_closed(const fs::path& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = FlushFileBuffers(h); CloseHandle(h); return ok;
}
Data read_json(const fs::path& p) {
    auto s = read_text(p); if (s.empty()) return data();
    auto d = Data(obs_data_create_from_json(s.c_str()), obs_data_release); return d ? std::move(d) : data();
}
void write_json(const fs::path& p, obs_data_t* d) { atomic_write(p, obs_data_get_json(d)); }
std::wstring quote_arg(const std::wstring& arg) {
    std::wstring out = L"\""; unsigned slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\'); slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\'); return out + L'\"';
}
static std::wstring command(const fs::path& exe, const std::vector<std::wstring>& args) {
    auto line = quote_arg(exe.wstring()); for (auto& arg : args) line += L" " + quote_arg(arg); return line;
}
DWORD run_process(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& log, DWORD timeout) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)}; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in; si.hStdOutput = out; si.hStdError = out;
    PROCESS_INFORMATION pi{}; auto line = command(exe, args);
    BOOL ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in); if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) throw std::runtime_error("Cannot launch " + path_text(exe));
    if (WaitForSingleObject(pi.hProcess, timeout) == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 124); WaitForSingleObject(pi.hProcess, 5000); }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return code;
}
void launch(const std::vector<std::wstring>& args) {
    fs::path exe = exe_dir() / "frinky-clip.exe"; auto line = command(exe, args);
    STARTUPINFOW si{sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) throw std::runtime_error("Could not launch recorder");
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
}
Config Config::load() {
    Config c; c.storage = app_dir() / "Recordings";
    auto d = read_json(app_dir() / "config.json");
    auto integer = [&](const char* k, int& v) { if (obs_data_has_user_value(d.get(), k)) v = (int)obs_data_get_int(d.get(), k); };
    integer("bitrate", c.bitrate); integer("max_bitrate", c.max_bitrate); integer("retention_minutes", c.retention_minutes);
    integer("save_seconds", c.save_seconds); integer("share_bitrate", c.share_bitrate);
    integer("export_height", c.export_height); integer("export_fps", c.export_fps);
    if (obs_data_has_user_value(d.get(), "budget_gb")) c.budget_gb = obs_data_get_double(d.get(), "budget_gb");
    if (obs_data_has_user_value(d.get(), "audio")) c.audio = obs_data_get_bool(d.get(), "audio");
    if (obs_data_has_user_value(d.get(), "record_on_launch")) c.record_on_launch = obs_data_get_bool(d.get(), "record_on_launch");
    if (obs_data_has_user_value(d.get(), "hotkey")) c.hotkey = (unsigned)obs_data_get_int(d.get(), "hotkey");
    if (obs_data_has_user_value(d.get(), "modifiers")) c.modifiers = (unsigned)obs_data_get_int(d.get(), "modifiers");
    c.monitor = obs_data_get_string(d.get(), "monitor");
    if (obs_data_has_user_value(d.get(), "export_codec")) c.export_codec = obs_data_get_string(d.get(), "export_codec");
    std::string p = obs_data_get_string(d.get(), "storage"); if (!p.empty()) c.storage = fs::path(wide(p));
    return c;
}
void Config::validate() const {
    if (bitrate < 1000 || max_bitrate < bitrate || max_bitrate > 200000 || retention_minutes < 1 || retention_minutes > 1440 ||
        !std::isfinite(budget_gb) || budget_gb < 0.1 || budget_gb > 10000 || save_seconds < 1 || save_seconds > retention_minutes * 60 ||
        share_bitrate < 1000 || share_bitrate > 100000 || hotkey < VK_F1 || hotkey > VK_F12 ||
        (export_height != 720 && export_height != 1080 && export_height != 1440) || (export_fps != 30 && export_fps != 60) ||
        (export_codec != "h264" && export_codec != "av1") ||
        (modifiers & ~(MOD_CONTROL | MOD_SHIFT | MOD_ALT)) || !storage.is_absolute()) throw std::runtime_error("Invalid settings: check bitrate, retention, storage budget, save duration and absolute storage path.");
}
void Config::save() const {
    validate(); auto d = data();
    obs_data_set_int(d.get(), "bitrate", bitrate); obs_data_set_int(d.get(), "max_bitrate", max_bitrate);
    obs_data_set_int(d.get(), "retention_minutes", retention_minutes); obs_data_set_double(d.get(), "budget_gb", budget_gb);
    obs_data_set_int(d.get(), "save_seconds", save_seconds); obs_data_set_int(d.get(), "share_bitrate", share_bitrate);
    obs_data_set_int(d.get(), "export_height", export_height); obs_data_set_int(d.get(), "export_fps", export_fps);
    obs_data_set_string(d.get(), "export_codec", export_codec.c_str());
    obs_data_set_int(d.get(), "hotkey", hotkey); obs_data_set_int(d.get(), "modifiers", modifiers);
    obs_data_set_bool(d.get(), "audio", audio); obs_data_set_string(d.get(), "monitor", monitor.c_str());
    obs_data_set_bool(d.get(), "record_on_launch", record_on_launch);
    obs_data_set_string(d.get(), "storage", path_text(storage).c_str()); write_json(app_dir() / "config.json", d.get());
}
HWND recorder_window() { return FindWindowW(RecorderClass, nullptr); }
std::vector<Monitor> monitors() {
    std::vector<Monitor> result;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi); GetMonitorInfoW(h, &mi);
        DISPLAY_DEVICEW dd{}; dd.cb = sizeof(dd); EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME);
        int w = mi.rcMonitor.right - mi.rcMonitor.left, height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        auto label = utf8(mi.szDevice) + "  " + std::to_string(w) + " x " + std::to_string(height);
        bool primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0; if (primary) label += " (primary)";
        reinterpret_cast<std::vector<Monitor>*>(p)->push_back({utf8(dd.DeviceID[0] ? dd.DeviceID : mi.szDevice), label, w, height, primary}); return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    std::stable_sort(result.begin(), result.end(), [](auto& a, auto& b) { return a.primary > b.primary; }); return result;
}
}
