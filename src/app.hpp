#pragma once
#include "common.hpp"
#include <shellapi.h>

namespace clip {
constexpr wchar_t AppWindowClass[] = L"FrinkyClip.Controls.0.1";
constexpr UINT StartMessage = WM_APP + 5;
constexpr UINT QuitMessage = WM_APP + 6;
constexpr UINT UpdateTestMessage = WM_APP + 14;

// The tray application owns the recording process and all of its helpers.
// The recorder process stays warm between recording sessions; Start and Stop
// are messages to it, and only Quit ends it. Hiding the controls never
// changes this lifetime.
class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    bool primary() const { return primary_; }
    bool active() const { return process_ != nullptr; } // Recorder process alive.
    bool quitting() const { return quitting_ || updating_; }
    bool updating() const { return updating_; }
    // One of: paused, starting, recording, stopping, quitting.
    const std::string& state() const { return state_; }
    bool recording() const { return state_ == "recording"; }
    bool paused() const { return state_ == "paused"; }
    DWORD recorder_pid() const { return pid_; }
    HWND recorder() const;
    void attach(HWND window);
    bool handle_message(UINT message, WPARAM w, LPARAM l);
    void warm();  // Launch an idle recorder so the first Start is fast.
    void start(); // Launch the recorder, or resume a paused one.
    void stop();
    void quit();
    void begin_update();
    void cancel_update(); // Safe after a failed update handoff; recorder stays stopped.
    bool tick(); // True only once quitting has left no worker processes.
    // Feed the recorder's reported state (empty when unknown/stale) each frame.
    void observe(const std::string& reported, bool busy, std::int64_t reported_ms, bool failed);
    bool start_requested = false;
    int update_action = 0; // Isolated integration tests use the same UI actions.
    bool status_changed = false; // The recorder rewrote status.json since the last read.
    bool index_changed = false;  // The recorder rewrote segments.json since the last read.
    std::string error;
private:
    enum class Pending { None, Start, Stop };
    HANDLE instance_ = nullptr, job_ = nullptr, process_ = nullptr;
    DWORD pid_ = 0;
    HWND window_ = nullptr;
    bool primary_ = false, quitting_ = false, busy_ = false;
    bool updating_ = false;
    Pending pending_ = Pending::None;
    std::int64_t pending_since_ = 0; ULONGLONG quit_started_ = 0;
    UINT taskbar_created_ = 0;
    NOTIFYICONDATAW tray_{sizeof(tray_)};
    std::wstring tray_text_;
    std::string state_ = "paused";
    bool add_tray();
    void spawn(bool idle);
    void update_tray();
};
}
