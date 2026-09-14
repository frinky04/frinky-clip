#include "updates.hpp"
#include "app.hpp"
#include "version.h"
#include <Velopack.hpp>
#include <mutex>
#include <thread>

namespace clip {
namespace {
constexpr auto Repository = "https://github.com/frinky04/frinky-clip";
// Fast installer callbacks must never enter the normal GUI/recorder path.
void close_for_installer(void*, const char*) {
    auto window = FindWindowW(AppWindowClass, nullptr);
    if (!window) return;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(window, QuitMessage, 0, 0);
    if (process) { WaitForSingleObject(process, 12000); CloseHandle(process); }
}
std::unique_ptr<Velopack::UpdateManager> manager() {
    wchar_t feed[32768]{};
    // A local feed is solely an integration-test facility, never a user setting.
    if (GetEnvironmentVariableW(L"FRINKY_CLIP_HOME", nullptr, 0) &&
        GetEnvironmentVariableW(L"FRINKY_CLIP_UPDATE_FEED", feed, 32768))
        return std::make_unique<Velopack::UpdateManager>(utf8(feed));
    return std::make_unique<Velopack::UpdateManager>(std::make_unique<Velopack::GithubSource>(Repository));
}
}
void update_startup() {
    Velopack::VelopackApp::Build().SetAutoApplyOnStartup(false)
        .OnBeforeUninstall([](void* context, const char* version) {
            close_for_installer(context, version);
            try { set_starts_with_windows(false); } catch (...) { /* Uninstall must still remove the app. */ }
        }).OnBeforeUpdate(close_for_installer).Run();
}
struct Updates::Work {
    std::mutex mutex;
    Status status;
    std::optional<Velopack::UpdateInfo> available;
    std::optional<Velopack::VelopackAsset> pending;
};
Updates::Updates() : work_(std::make_shared<Work>()), started_(steady_ms()) {
    try {
        // Unpackaged builds have no manifest. Avoid SDK initialization altogether.
        if (!fs::exists(exe_dir() / "sq.version")) return;
        auto sdk = manager();
        if (sdk->IsPortable()) return;
        work_->status.installed = true;
        work_->pending = sdk->UpdatePendingRestart();
        if (work_->pending) {
            work_->status.ready = true;
            work_->status.version = work_->pending->Version;
            work_->status.message = "Update downloaded";
        }
        auto saved = read_json(app_dir() / "updates.json");
        last_check_ = obs_data_get_int(saved.get(), "last_check_ms");
    } catch (const std::exception& e) { work_->status.error = e.what(); }
}
Updates::Status Updates::status() const {
    std::lock_guard lock(work_->mutex); return work_->status;
}
void Updates::tick(bool automatic) {
    auto state = status();
    const auto now = now_ms();
    if (automatic && state.installed && !state.working && !state.ready && !state.available &&
        steady_ms() - started_ > 15000 && (now - last_check_ >= 24ll * 60 * 60 * 1000 || last_check_ > now)) check(true);
    state = status();
    // Logging and test observation stay on the UI thread. Network tasks never
    // reference a window, App, ImGui, or another object destroyed by normal Quit.
    auto report = state.message + state.error + state.version + std::to_string(state.progress);
    if (report == last_report_) return;
    last_report_ = report;
    try {
        if (!state.error.empty()) atomic_write(app_dir() / "update-error.txt", state.error);
        if (GetEnvironmentVariableW(L"FRINKY_CLIP_HOME", nullptr, 0)) {
            auto d = data();
            obs_data_set_string(d.get(), "current_version", FRINKY_VERSION);
            obs_data_set_string(d.get(), "version", state.version.c_str());
            obs_data_set_string(d.get(), "message", state.message.c_str());
            obs_data_set_string(d.get(), "error", state.error.c_str());
            obs_data_set_bool(d.get(), "installed", state.installed);
            obs_data_set_bool(d.get(), "working", state.working);
            obs_data_set_bool(d.get(), "available", state.available);
            obs_data_set_bool(d.get(), "ready", state.ready);
            write_json_fast(app_dir() / "update-status.json", d.get());
        }
    } catch (...) { /* Diagnostics cannot stop recording or shutdown. */ }
}
void Updates::check() { check(false); }
void Updates::check(bool automatic) {
    if (steady_ms() < next_manual_) return;
    {
        std::lock_guard lock(work_->mutex);
        if (!work_->status.installed || work_->status.working || work_->status.ready) return;
        work_->status.working = true; work_->status.error.clear(); work_->status.message = "Checking for updates...";
    }
    next_manual_ = steady_ms() + 10000; last_check_ = now_ms();
    try {
        auto d = data(); obs_data_set_int(d.get(), "last_check_ms", last_check_); write_json(app_dir() / "updates.json", d.get());
    } catch (...) { /* Still throttle this process if preferences aren't writable. */ }
    // The native SDK has synchronous, non-cancellable network calls. A task owns
    // all of its data, so normal Quit never joins a stalled network request.
    // The OS ends any remaining network thread when the process exits.
    try {
        std::thread([work = work_, automatic] {
            try {
                auto sdk = manager(); auto found = sdk->CheckForUpdates();
                std::lock_guard lock(work->mutex);
                work->available = std::move(found);
                work->status.available = work->available.has_value();
                work->status.version = work->available ? work->available->TargetFullRelease.Version : "";
                work->status.message = work->available ? "Update available" : "You're up to date";
            } catch (const std::exception& e) {
                std::lock_guard lock(work->mutex); work->status.error = e.what();
                work->status.message = automatic ? "" : "Could not check for updates";
            }
            std::lock_guard lock(work->mutex); work->status.working = false;
        }).detach();
    } catch (const std::exception& e) {
        std::lock_guard lock(work_->mutex); work_->status.working = false; work_->status.error = e.what();
    }
}
void Updates::download() {
    std::optional<Velopack::UpdateInfo> update;
    {
        std::lock_guard lock(work_->mutex);
        if (work_->status.working || !work_->available || work_->status.ready) return;
        update = work_->available;
        work_->status.working = true; work_->status.progress = 0;
        work_->status.error.clear(); work_->status.message = "Downloading update...";
    }
    try {
        std::thread([work = work_, update = std::move(*update)] {
            try {
                auto sdk = manager();
                sdk->DownloadUpdates(update, [](void* value, size_t progress) {
                    auto* state = static_cast<Work*>(value); std::lock_guard lock(state->mutex);
                    state->status.progress = (int)progress;
                }, work.get());
                std::lock_guard lock(work->mutex);
                work->pending = update.TargetFullRelease;
                work->status.ready = true; work->status.message = "Update downloaded";
            } catch (const std::exception& e) {
                std::lock_guard lock(work->mutex); work->status.error = e.what();
                work->status.message = "Download failed. Try again.";
            }
            std::lock_guard lock(work->mutex); work->status.working = false;
        }).detach();
    } catch (const std::exception& e) {
        std::lock_guard lock(work_->mutex); work_->status.working = false; work_->status.error = e.what();
    }
}
void Updates::apply(bool resume_recording) {
    std::lock_guard lock(work_->mutex);
    if (!work_->status.ready || work_->status.working || !work_->pending) throw std::runtime_error("No downloaded update is ready.");
    // The tray process itself is outside the recorder job. Do not launch this
    // from a recorder/helper, whose job is killed during shutdown.
    auto sdk = manager();
    sdk->WaitExitThenApplyUpdates(*work_->pending, false, true,
        {resume_recording ? "--updated-recording" : "--updated-paused"});
}
}
