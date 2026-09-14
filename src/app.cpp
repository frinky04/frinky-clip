#include "app.hpp"
#include <stdexcept>

namespace clip {
App::App() {
    instance_ = CreateMutexW(nullptr, FALSE, L"Local\\FrinkyClip.App.0.1");
    if (!instance_) throw std::runtime_error("Cannot create the application lock");
    primary_ = GetLastError() != ERROR_ALREADY_EXISTS;
}
App::~App() {
    // Also covers exceptions: KILL_ON_JOB_CLOSE prevents orphaned recording.
    if (job_) CloseHandle(job_);
    if (process_) CloseHandle(process_);
    if (tray_.hWnd) Shell_NotifyIconW(NIM_DELETE, &tray_);
    if (instance_) CloseHandle(instance_);
}
bool App::add_tray() {
    return Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
}
void App::attach(HWND window) {
    window_ = window;
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_) throw std::runtime_error("Cannot create recorder lifetime group");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error("Cannot bind recording lifetime to the app");
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    tray_.hWnd = window; tray_.uID = 1; tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray_.uCallbackMessage = TrayMessage; tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray_.szTip, L"Frinky Clip - paused");
    if (!add_tray()) throw std::runtime_error("Cannot create the tray icon. Recording has not started.");
}
HWND App::recorder() const {
    auto window = recorder_window(); DWORD owner = 0;
    if (window) GetWindowThreadProcessId(window, &owner);
    return process_ && owner == pid_ ? window : nullptr;
}
void App::spawn(bool idle) {
    if (active() || quitting_) return;
    if (recorder_window()) throw std::runtime_error("An older recorder is still running. Stop it before starting this app's recorder.");
    Config::load().validate();
    auto exe = exe_dir() / "frinky-clip.exe";
    auto command = quote_arg(exe.wstring()) + L" --recorder --owned" + (idle ? L" --idle" : L"");
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) throw std::runtime_error("Could not launch recorder");
    // Assign before any child code runs, so even startup failure cannot orphan it.
    if (!AssignProcessToJobObject(job_, pi.hProcess) || ResumeThread(pi.hThread) == DWORD(-1)) {
        TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        throw std::runtime_error("Could not attach recorder to the tray app");
    }
    CloseHandle(pi.hThread); process_ = pi.hProcess; pid_ = pi.dwProcessId;
    pending_ = idle ? Pending::None : Pending::Start; pending_since_ = now_ms(); error.clear();
}
void App::warm() { spawn(true); }
void App::start() {
    if (quitting_) return;
    if (!active()) { spawn(false); return; }
    if (!paused()) return;
    Config::load().validate();
    if (auto worker = recorder()) {
        PostMessageW(worker, ResumeMessage, 0, 0);
        pending_ = Pending::Start; pending_since_ = now_ms(); error.clear();
    }
}
void App::stop() {
    if (!active() || paused()) return;
    if (auto worker = recorder()) { PostMessageW(worker, StopMessage, 0, 0); pending_ = Pending::Stop; pending_since_ = now_ms(); }
}
void App::quit() {
    if (quitting_) return;
    quitting_ = true; quit_started_ = GetTickCount64(); pending_ = Pending::None;
    if (auto worker = recorder()) PostMessageW(worker, ExitMessage, 0, 0);
}
bool App::tick() {
    if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
        DWORD code = 0; GetExitCodeProcess(process_, &code);
        if (code && !quitting_) error = "Recorder exited unexpectedly. See Log for details.";
        CloseHandle(process_); process_ = nullptr; pending_ = Pending::None;
        // The recorder normally waits for its mux/export helpers. Reap any
        // leftovers after an abnormal exit rather than leaving a background job.
        TerminateJobObject(job_, code);
    }
    if (!quitting_) return false;
    // Repeat in case the recorder window did not exist yet when Quit was chosen.
    if (auto worker = recorder()) PostMessageW(worker, ExitMessage, 0, 0);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr))
        return !active(); // Closing the owning job handle still kills all helpers.
    if (accounting.ActiveProcesses == 0) return true;
    // Allow the last segment and short saves to finish. Long exports retain
    // their source clips/partial outputs instead of keeping Quit open forever.
    if (GetTickCount64() - quit_started_ > 10000) TerminateJobObject(job_, 0);
    return false;
}
void App::observe(const std::string& reported, bool busy, std::int64_t reported_ms, bool failed) {
    // A live process with no fresh status of its own is still starting up.
    std::string state = !active() ? "paused" : reported.empty() ? "starting" : reported;
    if (state == "exiting") state = "quitting";
    // The recorder writes status right after handling a request, so a newer
    // report settles it; the timeout only covers a lost message.
    bool answered = reported_ms > pending_since_ && failed, expired = now_ms() - pending_since_ > 15000 || !active();
    if (pending_ == Pending::Start && (state != "paused" || answered || expired)) pending_ = Pending::None;
    if (pending_ == Pending::Stop && (state != "recording" || answered || expired)) pending_ = Pending::None;
    if (pending_ == Pending::Start) state = "starting";
    if (pending_ == Pending::Stop) state = "stopping";
    if (quitting_) state = "quitting";
    state_ = state; busy_ = busy; update_tray();
}
void App::update_tray() {
    std::wstring text = L"Frinky Clip - " + wide(state_);
    if (text == tray_text_) return;
    tray_text_ = text; wcscpy_s(tray_.szTip, text.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &tray_);
}
bool App::handle_message(UINT message, WPARAM w, LPARAM l) {
    if (message == WM_CLOSE) {
        ShowWindow(window_, SW_HIDE);
        return true;
    }
    if (message == QuitMessage || (message == WM_ENDSESSION && w)) { quit(); return true; }
    if (message == StartMessage) { if (!quitting_) start_requested = true; return true; }
    if (message == StatusMessage) { status_changed = true; return true; }
    if (message == taskbar_created_ && taskbar_created_) {
        if (!add_tray()) ShowWindow(window_, SW_SHOW);
        return true;
    }
    if (message != TrayMessage) return false;
    if (l == WM_LBUTTONUP || l == WM_LBUTTONDBLCLK) {
        ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_);
    } else if (l == WM_RBUTTONUP) {
        HMENU menu = CreatePopupMenu();
        bool settled = paused() || recording();
        AppendMenuW(menu, MF_STRING, 1, L"Open");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | ((quitting_ || !settled) ? MF_GRAYED : 0), 2, paused() ? L"Start Recording" : L"Stop Recording");
        AppendMenuW(menu, MF_STRING | ((!recording() || busy_ || quitting_) ? MF_GRAYED : 0), 3, L"Save Clip");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (quitting_ ? MF_GRAYED : 0), 4, quitting_ ? L"Quitting..." : L"Quit");
        POINT point; GetCursorPos(&point); SetForegroundWindow(window_);
        auto chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu); PostMessageW(window_, WM_NULL, 0, 0);
        if (chosen == 1) { ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); }
        if (chosen == 2) { if (paused()) start_requested = true; else stop(); }
        if (chosen == 3) if (auto worker = recorder()) PostMessageW(worker, SaveMessage, 0, 0);
        if (chosen == 4) quit();
    }
    return true;
}
}
