#include "recorder.hpp"
#include "buffer.hpp"
#include "media.hpp"
#include "export.hpp"
#include "app.hpp"
#include "updates.hpp"
#include <shellapi.h>
#include <objbase.h>
#include <dbghelp.h>
#include <cstdio>
#include <stdexcept>
extern "C" {
#include <libavutil/log.h>
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    clip::update_startup();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetDllDirectoryW(clip::exe_dir().c_str());
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exception) -> LONG {
        FILE* log = nullptr; _wfopen_s(&log, (clip::app_dir() / "crash.log").c_str(), L"w");
        if (!log) return EXCEPTION_EXECUTE_HANDLER;
        fprintf(log, "Exception %08lx at %p\n", exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress);
        HANDLE process = GetCurrentProcess();
        auto symbols = clip::path_text(clip::exe_dir()) + ";" + clip::path_text(clip::exe_dir() / "obs-plugins");
        SymInitialize(process, symbols.c_str(), TRUE);
        CONTEXT context = *exception->ContextRecord; STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 32; ++i) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 1024]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer); symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 1024;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) fprintf(log, "%s + 0x%llx\n", symbol->Name, displacement);
            else fprintf(log, "0x%llx\n", frame.AddrPC.Offset);
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        }
        fclose(log); SymCleanup(process); return EXCEPTION_EXECUTE_HANDLER;
    });
    av_log_set_level(AV_LOG_ERROR);
    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int result = 0;
    try {
        std::vector<std::wstring> args(argv + 1, argv + argc);
        if (!args.empty() && args[0] == L"--recorder") {
            bool synthetic = false, owned = false, idle = false; int seconds = 0;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == L"--synthetic") synthetic = true;
                else if (args[i] == L"--owned") owned = true;
                else if (args[i] == L"--idle") idle = true;
                else if (args[i] == L"--seconds" && i + 1 < args.size()) seconds = std::stoi(args[++i]);
            }
            BOOL in_job = FALSE;
            IsProcessInJob(GetCurrentProcess(), nullptr, &in_job);
            if (!synthetic && (!owned || !in_job || !FindWindowW(clip::AppWindowClass, nullptr)))
                throw std::runtime_error("Start Frinky Clip normally. Recording must be owned by the tray app.");
            if (synthetic && !GetEnvironmentVariableW(L"FRINKY_CLIP_HOME", nullptr, 0))
                throw std::runtime_error("Synthetic recording requires an isolated FRINKY_CLIP_HOME.");
            result = clip::run_recorder(synthetic, seconds, idle);
        } else if (!args.empty() && args[0] == L"--quit") {
            auto window = FindWindowW(clip::AppWindowClass, nullptr);
            result = window && PostMessageW(window, clip::QuitMessage, 0, 0) ? 0 : 1;
        } else if (!args.empty() && args[0] == L"--save") {
            auto window = clip::recorder_window();
            result = window && PostMessageW(window, clip::SaveMessage, args.size() > 1 ? std::stoi(args[1]) : 0, 0) ? 0 : 1;
        } else if (!args.empty() && args[0] == L"--stop") {
            auto window = clip::recorder_window(); result = window && PostMessageW(window, clip::StopMessage, 0, 0) ? 0 : 1;
        } else if (!args.empty() && args[0] == L"--start") {
            auto window = clip::recorder_window(); result = window && PostMessageW(window, clip::ResumeMessage, 0, 0) ? 0 : 1;
        } else if (!args.empty() && args[0] == L"--export" && args.size() >= 3) {
            // Export [start_ms, end_ms] epoch times with the saved export defaults; the UI writes the same request file.
            auto cfg = clip::Config::load(); clip::ExportRequest request;
            request.start_ms = std::stoll(args[1]); request.end_ms = std::stoll(args[2]);
            request.height = cfg.export_height; request.fps = cfg.export_fps; request.bitrate_kbps = cfg.share_bitrate; request.codec = cfg.export_codec; request.audio = cfg.desktop_gain > 0; request.mic = cfg.mic && cfg.mic_gain > 0;
            request.desktop_gain = cfg.desktop_gain / 100.0; request.mic_gain = cfg.mic_gain / 100.0;
            clip::write_export_request(clip::app_dir() / "export-request.json", request);
            auto window = clip::recorder_window(); result = window && PostMessageW(window, clip::ExportMessage, 0, 0) ? 0 : 1;
        } else if (!args.empty() && args[0] == L"--export-test" && args.size() >= 2) {
            // Headless check of the in-process export over a buffer folder;
            // writes export-test.txt. Optional second argument: clip seconds.
            result = clip::export_test(clip::fs::path(args[1]), args.size() > 2 ? std::stoi(args[2]) : 3);
        } else if (!args.empty() && args[0] == L"--recover") {
            if (clip::recorder_window()) throw std::runtime_error("Stop the recorder before standalone recovery.");
            auto cfg = clip::Config::load(); cfg.validate(); clip::Buffer buffer(cfg); buffer.recover();
            auto d = clip::data(); obs_data_set_int(d.get(), "segments", buffer.segments().size());
            obs_data_set_int(d.get(), "recovered", buffer.recovered); obs_data_set_int(d.get(), "quarantined", buffer.quarantined);
            obs_data_set_double(d.get(), "seconds", buffer.seconds()); clip::write_json(clip::app_dir() / "recovery.json", d.get());
        } else if (!args.empty() && args[0] == L"--probe-frame" && args.size() >= 3) {
            // Diagnostic: decode one frame and report timing to frame-probe.txt.
            auto began = clip::now_ms(); std::string report;
            for (bool keyframe : {true, false}) {
                try {
                    auto frame = clip::decode_frame(clip::fs::path(args[1]), std::stoll(args[2]), 320, keyframe);
                    report += std::string(keyframe ? "keyframe" : "exact") + ": " + std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                        " pts " + std::to_string(frame.pts_ms) + " ms in " + std::to_string(clip::now_ms() - began) + " ms\n";
                } catch (const std::exception& e) { report += std::string(keyframe ? "keyframe" : "exact") + " failed: " + e.what() + "\n"; }
                began = clip::now_ms();
            }
            clip::atomic_write(clip::app_dir() / "frame-probe.txt", report);
        } else if (!args.empty() && args[0] == L"--player-test" && args.size() >= 2) {
            // Headless benchmark of seeking and playback over a buffer folder;
            // writes player-test.txt. Optional second argument: seconds to play.
            result = clip::player_test(clip::fs::path(args[1]), args.size() > 2 ? std::stoi(args[2]) : 5);
        } else if (!args.empty() && args[0] == L"--remux" && args.size() >= 3) {
            std::vector<clip::fs::path> sources; for (size_t i = 2; i < args.size(); ++i) sources.emplace_back(args[i]);
            clip::remux(sources, clip::fs::path(args[1]));
        } else if (!args.empty() && args[0] == L"--update-test" && args.size() == 2) {
            if (!GetEnvironmentVariableW(L"FRINKY_CLIP_HOME", nullptr, 0)) throw std::runtime_error("Update tests require isolated storage.");
            int action = args[1] == L"check" ? 1 : args[1] == L"download" ? 2 : args[1] == L"apply" ? 3 : 0;
            auto window = FindWindowW(clip::AppWindowClass, nullptr);
            result = action && window && PostMessageW(window, clip::UpdateTestMessage, action, 0) ? 0 : 1;
        } else result = clip::run_ui(!args.empty() && args[0] == L"--updated-recording" ? 1 : !args.empty() && args[0] == L"--updated-paused" ? 0 : -1);
    } catch (const std::exception& e) {
        if (argc == 1) MessageBoxW(nullptr, clip::wide(e.what()).c_str(), L"Frinky Clip", MB_OK | MB_ICONERROR);
        else { try { clip::atomic_write(clip::app_dir() / "command-error.txt", e.what()); } catch (...) {} }
        result = 1;
    }
    LocalFree(argv); CoUninitialize(); return result;
}
