#include "recorder.hpp"
#include "buffer.hpp"
#include "timeline.hpp"
#include <shellapi.h>
#include <future>
#include <mutex>
#include <atomic>
#include <deque>
#include <stdexcept>
#include <algorithm>
#include <cstdio>
#include <share.h>
#include <util/base.h>

namespace clip {
namespace {
struct SaveJob { fs::path folder, output; std::vector<fs::path> paths; };
// The recorder process stays warm between recording sessions: the OBS core,
// the graphics device, and the plugin modules are initialized once, while
// capture sources, encoders, and the output only exist during a session.
class Recorder {
public:
    Config cfg = Config::load();
    Buffer buffer{cfg};
    HWND window = nullptr;
    bool synthetic = false, initialized = false, stopping = false, exiting = false, hotkey_registered = false;
    bool waiting_save = false, smoke_saved = false;
    int run_seconds = 0, exit_code = 0;
    std::int64_t started = 0, last_status = 0, stop_started = 0;
    fs::path active, session_dir, pending_last;
    std::vector<fs::path> to_finalize, pinned;
    std::mutex events_mutex;
    std::deque<fs::path> changes;
    std::atomic<int> stopped_code{-999};
    obs_source_t* capture = nullptr;
    obs_source_t* audio = nullptr;
    obs_scene_t* scene = nullptr;
    obs_encoder_t* video_encoder = nullptr;
    obs_encoder_t* audio_encoder = nullptr;
    obs_output_t* output = nullptr;
    std::string message = "Starting recorder...", last_clip, failure;
    std::future<std::string> worker;
    std::deque<SaveJob> recovered_jobs;
    std::uint64_t frame_count = 0, lagged_frames = 0, skipped_frames = 0, render_frames = 0;
    double render_ms = 0;
    // Editor protection: footage in [pin_start, pin_end] stays while the pin is
    // refreshed. An export additionally hard-links its sources.
    std::int64_t pin_start = 0, pin_end = 0, pin_at = 0;
    size_t indexed_count = (size_t)-1; std::int64_t indexed_end = -1; // Last published segment index.
    fs::path export_progress_file; std::int64_t export_duration_ms = 0; double export_fraction = -1;

    ~Recorder() {
        if (worker.valid()) worker.wait();
        shutdown_core();
        if (window) DestroyWindow(window);
    }
    bool session() const { return output != nullptr; }
    bool recording() const { return output && obs_output_active(output) && !stopping; }
    void load_module(const fs::path& root, const char* name) {
        obs_module_t* module = nullptr;
        auto binary = path_text(root / "obs-plugins" / "64bit" / (std::string(name) + ".dll"));
        auto module_data = path_text(root / "data" / "obs-plugins" / name);
        if (obs_open_module(&module, binary.c_str(), module_data.c_str()) != MODULE_SUCCESS || !obs_init_module(module))
            throw std::runtime_error(std::string("Cannot load OBS module: ") + name);
    }
    void load_pending() {
        // A previously protected save can finish even after its original ring
        // entries have expired. No pending files are deleted on startup.
        for (auto& dir : fs::directory_iterator(cfg.storage / "pending")) {
            if (!dir.is_directory() || !dir.path().filename().wstring().starts_with(L"clip-") ||
                (GetFileAttributesW(dir.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) || !fs::exists(dir.path() / "request.json")) continue;
            auto d = read_json(dir.path() / "request.json");
            auto name = fs::path(wide(obs_data_get_string(d.get(), "filename"))).filename();
            if (name.empty() || name.extension() != L".mp4") continue;
            SaveJob job{dir.path(), cfg.storage / "clips" / name, {}};
            for (auto& f : fs::directory_iterator(dir.path())) if (f.is_regular_file() && f.path().extension() == L".mkv") job.paths.push_back(f.path());
            std::sort(job.paths.begin(), job.paths.end());
            if (!job.paths.empty()) recovered_jobs.push_back(std::move(job));
        }
    }
    void load_last_clip() {
        auto previous = read_json(app_dir() / "status.json");
        auto previous_clip = fs::path(wide(obs_data_get_string(previous.get(), "last_clip")));
        std::error_code clip_error;
        if (previous_clip.parent_path() == cfg.storage / "clips" && fs::is_regular_file(previous_clip, clip_error))
            last_clip = path_text(previous_clip);
    }
    // Process start: recover the buffer and bring up the OBS core. Fatal on failure.
    void start() {
        auto launched = now_ms();
        cfg.validate();
        load_last_clip();
        buffer.recover(); load_pending(); buffer.prune();
        auto runtime = read_json(exe_dir() / "runtime.json");
        auto root = fs::path(wide(obs_data_get_string(runtime.get(), "obs_root")));
        if (root.empty()) throw std::runtime_error("Missing runtime.json. Run scripts/build.ps1.");
        auto core_data = path_text(root / "data" / "libobs") + "/"; obs_add_data_path(core_data.c_str());
        if (!obs_startup("en-US", path_text(app_dir() / "obs-config").c_str(), nullptr)) throw std::runtime_error("OBS initialization failed");
        initialized = true;
        if (obs_get_version() != LIBOBS_API_VER) throw std::runtime_error("OBS runtime version changed. Rebuild with matching headers.");
        obs_audio_info ai{48000, SPEAKERS_STEREO};
        if (!obs_reset_audio(&ai)) throw std::runtime_error("Audio initialization failed");
        obs_video_info vi{};
        auto graphics = path_text(exe_dir() / "libobs-d3d11.dll"); vi.graphics_module = graphics.c_str();
        vi.fps_num = 60; vi.fps_den = 1; vi.base_width = 2560; vi.base_height = 1440;
        vi.output_width = 2560; vi.output_height = 1440; vi.output_format = VIDEO_FORMAT_NV12;
        vi.gpu_conversion = true; vi.colorspace = VIDEO_CS_709; vi.range = VIDEO_RANGE_PARTIAL; vi.scale_type = OBS_SCALE_BICUBIC;
        if (obs_reset_video(&vi) != OBS_VIDEO_SUCCESS) throw std::runtime_error("Direct3D video initialization failed");
        load_module(root, "obs-ffmpeg"); load_module(root, "obs-nvenc");
        load_module(root, synthetic ? "image-source" : "win-capture");
        // Audio can be enabled between sessions, so the module is always available.
        if (!synthetic) load_module(root, "win-wasapi");
        obs_post_load_modules();
        blog(LOG_INFO, "Frinky Clip: core ready in %lld ms with %zu buffered segments", (long long)(now_ms() - launched), buffer.segments().size());
        message = "Paused";
    }
    // Session start: apply the current settings and begin capturing. Recoverable on failure.
    void begin_session() {
        if (session()) return;
        auto fresh = Config::load(); fresh.validate();
        if (fresh.storage != cfg.storage) {
            cfg = fresh; buffer = Buffer(cfg); buffer.recover(); load_pending(); load_last_clip();
        } else { cfg = fresh; buffer.configure(cfg); }
        buffer.prune();
        failure.clear(); stopping = false; stop_started = 0; stopped_code = -999;
        frame_count = lagged_frames = skipped_frames = render_frames = 0; render_ms = 0;
        auto displays = monitors();
        if (displays.empty()) throw std::runtime_error("No monitor available");
        auto selected = displays.front();
        if (!cfg.monitor.empty()) {
            auto it = std::find_if(displays.begin(), displays.end(), [&](auto& m) { return m.id == cfg.monitor; });
            if (it == displays.end()) throw std::runtime_error("Selected monitor is disconnected. Select an available monitor.");
            selected = *it;
        }
        auto settings = data();
        if (synthetic) {
            obs_data_set_int(settings.get(), "width", 2560); obs_data_set_int(settings.get(), "height", 1440);
            obs_data_set_int(settings.get(), "color", 0xff4080c0);
            capture = obs_source_create("color_source_v3", "Test pattern", settings.get(), nullptr);
        } else {
            obs_data_set_string(settings.get(), "monitor_id", selected.id.c_str());
            obs_data_set_int(settings.get(), "method", 1); // OBS Desktop Duplication, GPU texture capture.
            obs_data_set_bool(settings.get(), "capture_cursor", true); obs_data_set_bool(settings.get(), "force_sdr", true);
            capture = obs_source_create("monitor_capture", "Display", settings.get(), nullptr);
        }
        if (!capture) throw std::runtime_error("Display capture creation failed");
        scene = obs_scene_create_private("Capture canvas");
        auto* item = obs_scene_add(scene, capture);
        vec2 bounds{2560.f, 1440.f}; obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER); obs_sceneitem_set_bounds(item, &bounds);
        obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
        obs_set_output_source(0, obs_scene_get_source(scene));
        if (cfg.audio && !synthetic) {
            settings = data(); obs_data_set_string(settings.get(), "device_id", "default");
            obs_data_set_bool(settings.get(), "use_device_timing", true);
            audio = obs_source_create("wasapi_output_capture", "Desktop audio", settings.get(), nullptr);
            if (!audio) throw std::runtime_error("Desktop audio capture creation failed");
            obs_set_output_source(1, audio);
        }
        // ffmpeg_muxer requires an audio encoder. With desktop capture disabled,
        // OBS supplies silence; no microphone or application audio is captured.
        settings = data(); obs_data_set_int(settings.get(), "bitrate", 192);
        audio_encoder = obs_audio_encoder_create("ffmpeg_aac", "AAC", settings.get(), 0, nullptr);
        if (!audio_encoder) throw std::runtime_error("AAC encoder unavailable");
        obs_encoder_set_audio(audio_encoder, obs_get_audio());
        settings = data(); obs_data_set_string(settings.get(), "rate_control", "vbr");
        obs_data_set_int(settings.get(), "bitrate", cfg.bitrate); obs_data_set_int(settings.get(), "max_bitrate", cfg.max_bitrate);
        obs_data_set_int(settings.get(), "keyint_sec", 2); obs_data_set_string(settings.get(), "preset", "p4");
        obs_data_set_string(settings.get(), "multipass", "disabled"); obs_data_set_bool(settings.get(), "lookahead", false);
        obs_data_set_bool(settings.get(), "adaptive_quantization", false); obs_data_set_int(settings.get(), "bf", 0);
        video_encoder = obs_video_encoder_create("obs_nvenc_av1_tex", "NVENC AV1", settings.get(), nullptr);
        if (!video_encoder) throw std::runtime_error("NVENC AV1 unavailable. Check the NVIDIA driver.");
        obs_encoder_set_video(video_encoder, obs_get_video());
        session_dir = buffer.root() / ("session-" + unique_id()); fs::create_directories(session_dir);
        active = session_dir / "segment-000000.mkv";
        settings = data(); obs_data_set_string(settings.get(), "path", path_text(active).c_str());
        obs_data_set_string(settings.get(), "directory", path_text(session_dir).c_str());
        obs_data_set_string(settings.get(), "format", "segment-%CCYY%MM%DD-%hh%mm%ss");
        obs_data_set_string(settings.get(), "extension", "mkv"); obs_data_set_bool(settings.get(), "split_file", true);
        obs_data_set_int(settings.get(), "max_time_sec", 4); obs_data_set_bool(settings.get(), "allow_overwrite", false);
        output = obs_output_create("ffmpeg_muxer", "Disk buffer", settings.get(), nullptr);
        if (!output) throw std::runtime_error("Recording output unavailable");
        obs_output_set_video_encoder(output, video_encoder); if (audio_encoder) obs_output_set_audio_encoder(output, audio_encoder, 0);
        auto* signals = obs_output_get_signal_handler(output);
        signal_handler_connect(signals, "file_changed", [](void* p, calldata_t* cd) {
            auto* self = static_cast<Recorder*>(p); std::lock_guard lock(self->events_mutex);
            self->changes.emplace_back(wide(calldata_string(cd, "next_file")));
        }, this);
        signal_handler_connect(signals, "stop", [](void* p, calldata_t* cd) {
            static_cast<Recorder*>(p)->stopped_code = (int)calldata_int(cd, "code");
        }, this);
        if (!obs_output_start(output)) {
            auto* reason = obs_output_get_last_error(output);
            throw std::runtime_error(std::string("Recording failed: ") + (reason ? reason : "encoder/output initialization failed; see recorder.log"));
        }
        started = now_ms(); message = "Recording";
        hotkey_registered = RegisterHotKey(window, 1, cfg.modifiers | MOD_NOREPEAT, cfg.hotkey) != FALSE;
    }
    // Session end: release everything the session created and go idle.
    void end_session() {
        if (!initialized) return;
        if (hotkey_registered) { UnregisterHotKey(window, 1); hotkey_registered = false; }
        if (output) {
            if (obs_output_active(output)) obs_output_force_stop(output);
            obs_output_release(output); output = nullptr;
        }
        obs_set_output_source(0, nullptr); obs_set_output_source(1, nullptr);
        if (scene) { obs_scene_release(scene); scene = nullptr; }
        if (capture) { obs_source_release(capture); capture = nullptr; }
        if (audio) { obs_source_release(audio); audio = nullptr; }
        if (video_encoder) { obs_encoder_release(video_encoder); video_encoder = nullptr; }
        if (audio_encoder) { obs_encoder_release(audio_encoder); audio_encoder = nullptr; }
        // Destroying the capture source stops desktop duplication, so an idle
        // recorder only renders an empty canvas.
        while (obs_wait_for_destroy_queue()) {}
        { std::lock_guard lock(events_mutex); changes.clear(); }
        active.clear(); session_dir.clear(); stopping = false; stop_started = 0; stopped_code = -999;
    }
    void shutdown_core() {
        if (!initialized) return;
        end_session();
        obs_shutdown(); initialized = false;
    }
    void resume() {
        if (exiting || session()) return;
        auto begun = now_ms();
        try { begin_session(); blog(LOG_INFO, "Frinky Clip: session started in %lld ms", (long long)(now_ms() - begun)); }
        catch (const std::exception& e) {
            end_session();
            failure = e.what(); message = "Recorder could not start.";
        }
    }
    bool busy() const { return waiting_save || worker.valid() || !recovered_jobs.empty(); }
    void request_save(int seconds) {
        if (busy()) { message = "A save or export is already in progress."; return; }
        if (seconds <= 0 || seconds > cfg.retention_minutes * 60) seconds = cfg.save_seconds;
        pinned.clear();
        // The current open segment contributes to the requested duration. Keep
        // one extra closed segment rather than risk dropping the requested start.
        for (auto i : select_recent(buffer.segments(), seconds, path_text(session_dir.filename()))) pinned.push_back(buffer.segments()[i].path);
        if (!active.empty() && !stopping) {
            pinned.push_back(active); pending_last = active; waiting_save = true;
            calldata_t cd{}; proc_handler_call(obs_output_get_proc_handler(output), "split_file", &cd); calldata_free(&cd);
            message = "Saving: waiting for the next keyframe...";
        } else if (!pinned.empty()) { begin_save(); }
        else message = "No completed footage in this session yet.";
    }
    void start_job(SaveJob job) {
        message = "Saving clip...";
        worker = std::async(std::launch::async, [job] {
            if (!fs::exists(job.output)) remux(job.paths, job.output);
            else inspect_media(job.output); // Previous process may have published before crashing.
            for (auto& p : job.paths) { std::error_code ec; fs::remove(p, ec); }
            std::error_code ec; fs::remove(job.folder / "request.json", ec); fs::remove(job.folder, ec);
            return path_text(job.output);
        });
    }
    void begin_save() {
        auto id = "clip-" + unique_id(); SaveJob job;
        job.folder = cfg.storage / "pending" / id; job.output = cfg.storage / "clips" / (id + ".mp4");
        job.paths = buffer.protect(pinned, job.folder);
        auto d = data(); obs_data_set_string(d.get(), "filename", path_text(job.output.filename()).c_str());
        write_json(job.folder / "request.json", d.get());
        pinned.clear(); waiting_save = false; pending_last.clear(); start_job(std::move(job));
    }
    void share() {
        if (busy() || last_clip.empty()) { message = "Save a clip first, then export it."; return; }
        wchar_t found[32768];
        if (!SearchPathW(nullptr, L"ffmpeg.exe", nullptr, 32768, found, nullptr)) { message = "Share export requires ffmpeg.exe on PATH."; return; }
        fs::path ffmpeg(found), input = fs::path(wide(last_clip));
        auto target = input.parent_path() / (input.stem().wstring() + L"-share-" + wide(unique_id()) + L".mp4");
        int bitrate = cfg.share_bitrate; message = "Exporting H.264 share copy...";
        worker = std::async(std::launch::async, [ffmpeg, input, target, bitrate] {
            auto temp = target; temp += L".partial";
            auto log = target; log += L".log";
            auto code = run_process(ffmpeg, {L"-nostdin", L"-hide_banner", L"-y", L"-i", input.wstring(), L"-map", L"0:v:0", L"-map", L"0:a?",
                L"-c:v", L"h264_nvenc", L"-preset", L"p4", L"-b:v", std::to_wstring(bitrate) + L"k", L"-maxrate", std::to_wstring(bitrate * 12 / 10) + L"k",
                L"-bufsize", std::to_wstring(bitrate * 2) + L"k", L"-c:a", L"aac", L"-b:a", L"192k", L"-movflags", L"+faststart", L"-f", L"mp4", temp.wstring()}, log);
            if (code) throw std::runtime_error("Share export failed. See " + path_text(log));
            inspect_media(temp);
            if (!flush_closed(temp) || !MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot publish share export");
            return path_text(target);
        });
    }
    void pin(std::int64_t start_ms, std::int64_t end_ms) {
        pin_start = start_ms; pin_end = end_ms; pin_at = (start_ms || end_ms) ? now_ms() : 0;
    }
    std::vector<Span> spans() const {
        std::vector<Span> result;
        for (auto& s : buffer.segments()) result.push_back({s.path, s.session, s.end_ms - (std::int64_t)std::llround(s.seconds * 1000), s.end_ms});
        return result;
    }
    // Export a range of the buffer as a re-encoded clip through ffmpeg.exe.
    void export_clip() {
        if (busy()) { message = "A save or export is already in progress."; return; }
        auto request = read_export_request(app_dir() / "export-request.json");
        if (request.end_ms - request.start_ms < 100) { message = "Mark a range of at least 0.1 s."; return; }
        auto sources = spans_in_range(spans(), request.start_ms, request.end_ms);
        if (sources.empty()) { message = "The range has no closed footage, or crosses a Stop/Record boundary."; return; }
        wchar_t found[32768];
        if (!SearchPathW(nullptr, L"ffmpeg.exe", nullptr, 32768, found, nullptr)) { message = "Export requires ffmpeg.exe on PATH."; return; }
        fs::path ffmpeg(found);
        auto id = "export-" + unique_id(); auto folder = cfg.storage / "pending" / id;
        std::vector<fs::path> paths; for (auto& s : sources) paths.push_back(s.path);
        auto linked = buffer.protect(paths, folder);
        std::string list; for (auto& p : linked) list += "file '" + path_text(p) + "'\n";
        atomic_write(folder / "list.txt", list);
        auto name = clip_name(request.start_ms); auto output = cfg.storage / "clips" / (name + ".mp4");
        for (int n = 2; fs::exists(output); ++n) output = cfg.storage / "clips" / (name + "-" + std::to_string(n) + ".mp4");
        auto temp = output; temp += L".partial"; auto log = folder / "ffmpeg.log";
        export_progress_file = folder / "progress.txt"; export_duration_ms = request.end_ms - request.start_ms; export_fraction = 0;
        auto args = export_args(request, sources.front().start_ms, folder / "list.txt", export_progress_file, temp);
        message = "Exporting clip...";
        worker = std::async(std::launch::async, [ffmpeg, args, log, temp, output, folder] {
            auto code = run_process(ffmpeg, args, log, 30 * 60 * 1000);
            if (code) throw std::runtime_error("Export failed. See " + path_text(log));
            inspect_media(temp);
            if (!flush_closed(temp) || !MoveFileExW(temp.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot publish exported clip");
            std::error_code ec; fs::remove_all(folder, ec);
            return path_text(output);
        });
    }
    void stop() {
        if (!session() || stopping) return;
        stopping = true; stop_started = now_ms(); message = "Stopping recorder...";
        if (obs_output_active(output)) obs_output_stop(output);
        else stopped_code = 0;
    }
    void exit() {
        if (exiting) return;
        exiting = true; stop();
        if (!session()) message = "Quitting...";
    }
    const char* state() const {
        return exiting ? "exiting" : stopping ? "stopping" : recording() ? "recording" : "paused";
    }
    void status() {
        auto d = data(); obs_data_set_int(d.get(), "updated_ms", now_ms()); obs_data_set_int(d.get(), "pid", GetCurrentProcessId());
        obs_data_set_string(d.get(), "state", state());
        obs_data_set_bool(d.get(), "recording", recording());
        obs_data_set_bool(d.get(), "busy", busy()); obs_data_set_string(d.get(), "message", message.c_str());
        obs_data_set_bool(d.get(), "hotkey_registered", hotkey_registered);
        obs_data_set_string(d.get(), "error", failure.c_str()); obs_data_set_string(d.get(), "last_clip", last_clip.c_str());
        obs_data_set_double(d.get(), "buffer_seconds", buffer.seconds()); obs_data_set_double(d.get(), "buffer_gb", buffer.bytes() / 1e9);
        obs_data_set_int(d.get(), "segments", (long long)buffer.segments().size());
        obs_data_set_int(d.get(), "frames", frame_count); obs_data_set_int(d.get(), "lagged_frames", lagged_frames);
        obs_data_set_int(d.get(), "skipped_frames", skipped_frames); obs_data_set_int(d.get(), "render_frames", render_frames);
        obs_data_set_double(d.get(), "render_ms", render_ms);
        obs_data_set_double(d.get(), "fps", recording() ? obs_get_active_fps() : 0);
        obs_data_set_double(d.get(), "export_progress", export_fraction);
        obs_data_set_int(d.get(), "recovered", buffer.recovered); obs_data_set_int(d.get(), "quarantined", buffer.quarantined);
        write_json(app_dir() / "status.json", d.get()); last_status = now_ms();
    }
    void tick() {
        {
            std::lock_guard lock(events_mutex);
            while (!changes.empty()) {
                if (!active.empty()) to_finalize.push_back(active);
                active = changes.front(); changes.pop_front();
            }
        }
        auto code = stopped_code.exchange(-999);
        if (code != -999 && session()) {
            stopping = true;
            if (!stop_started) stop_started = now_ms();
            if (code != 0) { failure = "Recording stopped unexpectedly (OBS code " + std::to_string(code) + "). See recorder.log."; exit_code = 1; }
            if (!active.empty()) { to_finalize.push_back(active); active.clear(); }
        }
        for (auto it = to_finalize.begin(); it != to_finalize.end();) {
            bool finalized = false;
            try { finalized = buffer.finalize(*it); }
            catch (const std::exception& e) {
                failure = e.what();
                // Leave damaged segments in place for next startup recovery.
                if (waiting_save && *it == pending_last) { waiting_save = false; pinned.clear(); }
                it = to_finalize.erase(it); continue;
            }
            if (!finalized && stopping && now_ms() - stop_started > 8000) {
                if (waiting_save && *it == pending_last) { waiting_save = false; pinned.clear(); failure = "The last segment did not close; retained for recovery."; }
                it = to_finalize.erase(it); continue;
            }
            if (finalized) it = to_finalize.erase(it); else ++it;
        }
        if (waiting_save && std::any_of(buffer.segments().begin(), buffer.segments().end(), [&](auto& s) { return s.path == pending_last; })) begin_save();
        if (worker.valid() && worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try { last_clip = worker.get(); message = (export_progress_file.empty() ? "Saved " : "Exported ") + path_text(fs::path(wide(last_clip)).filename()); }
            catch (const std::exception& e) { failure = e.what(); message = "Save failed; protected footage has been retained."; }
            export_fraction = -1; export_progress_file.clear();
        }
        if (!worker.valid() && !waiting_save && !recovered_jobs.empty()) { auto job = std::move(recovered_jobs.front()); recovered_jobs.pop_front(); start_job(std::move(job)); }
        if (worker.valid() && !export_progress_file.empty()) {
            if (auto fraction = export_progress(read_text(export_progress_file), export_duration_ms)) {
                export_fraction = *fraction; message = "Exporting clip... " + std::to_string((int)std::lround(*fraction * 100)) + "%";
            }
        }
        std::set<fs::path> protected_set(pinned.begin(), pinned.end());
        // The editor's view and marked range stay until the pin goes stale.
        if (pin_at && now_ms() - pin_at < 15000)
            for (auto& s : buffer.segments()) if (s.end_ms > pin_start && s.end_ms - s.seconds * 1000 < pin_end) protected_set.insert(s.path);
        buffer.prune(protected_set);
        if (recording()) {
            frame_count = obs_output_get_total_frames(output); lagged_frames = obs_get_lagged_frames();
            skipped_frames = video_output_get_skipped_frames(obs_get_video()); render_frames = obs_get_total_frames();
            render_ms = obs_get_average_frame_time_ns() / 1e6;
            auto free = fs::space(cfg.storage).available;
            if (free < 512ull * 1024 * 1024) { failure = "Recording stopped: less than 512 MB free on the storage drive."; stop(); }
            if (synthetic && capture) { auto d = data(); obs_data_set_int(d.get(), "color", 0xff000000 | ((now_ms() / 250 * 1234567) & 0xffffff)); obs_source_update(capture, d.get()); }
            if (run_seconds > 0) {
                if (!smoke_saved && now_ms() - started > 6500) { smoke_saved = true; request_save(5); }
                if (now_ms() - started > run_seconds * 1000) exit();
            }
        }
        if (stopping && output && obs_output_active(output) && now_ms() - stop_started > 5000) obs_output_force_stop(output);
        // The session ends once its last segment is closed. Saves continue in
        // the background; only Quit waits for them.
        if (stopping && (!output || !obs_output_active(output)) && to_finalize.empty() && !waiting_save) {
            auto requested = stop_started; end_session();
            blog(LOG_INFO, "Frinky Clip: session stopped in %lld ms", (long long)(now_ms() - requested));
            message = exiting ? "Quitting..." : failure.empty() ? "Paused" : "Recorder stopped. Completed buffer footage is retained.";
            status();
        }
        if (exiting && !session() && !busy()) {
            if (failure.empty()) message = "Recorder stopped. Completed buffer footage is retained.";
            shutdown_core(); status(); PostQuitMessage(exit_code); return;
        }
        if (now_ms() - last_status >= 1000) status();
        // Publish the segment list whenever it changes; the editor reads this
        // one file instead of every sidecar.
        auto& segments = buffer.segments();
        std::int64_t newest = segments.empty() ? 0 : segments.back().end_ms;
        if (segments.size() != indexed_count || newest != indexed_end) {
            BufferMap map; for (auto& s : spans()) map.spans.push_back(s);
            map.last_end_ms = newest;
            try { write_index(app_dir() / "segments.json", map); indexed_count = segments.size(); indexed_end = newest; } catch (...) {}
        }
    }
};
LRESULT CALLBACK recorder_proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    auto* r = reinterpret_cast<Recorder*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) { r = static_cast<Recorder*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(r)); }
    if (!r) return DefWindowProcW(window, msg, w, l);
    try {
        if (msg == SaveMessage || msg == WM_HOTKEY) { r->request_save(msg == WM_HOTKEY ? r->cfg.save_seconds : (int)w); return 0; }
        if (msg == StopMessage) { r->stop(); r->status(); return 0; }
        if (msg == ResumeMessage) { r->resume(); r->status(); return 0; }
        if (msg == ExitMessage || msg == WM_CLOSE || msg == WM_ENDSESSION) { r->exit(); r->status(); return 0; }
        if (msg == ShareMessage) { r->share(); return 0; }
        if (msg == PinMessage) { r->pin((std::int64_t)w, (std::int64_t)l); return 0; }
        if (msg == ExportMessage) { r->export_clip(); r->status(); return 0; }
        if (msg == WM_TIMER) { r->tick(); return 0; }
    } catch (const std::exception& e) { r->failure = e.what(); r->waiting_save = false; r->pinned.clear(); r->stop(); }
    return DefWindowProcW(window, msg, w, l);
}
}
int run_recorder(bool synthetic, int seconds, bool idle) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\FrinkyClip.Recorder.0.1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    fs::create_directories(app_dir());
    FILE* log = _wfsopen((app_dir() / "recorder.log").c_str(), L"w", _SH_DENYNO);
    base_set_log_handler([](int level, const char* fmt, va_list args, void* p) {
        if (level > LOG_INFO || !p) return;
        auto* file = static_cast<FILE*>(p); vfprintf(file, fmt, args); fputc('\n', file); fflush(file);
    }, log);
    int result = 0;
    try {
        Recorder r; r.synthetic = synthetic; r.run_seconds = seconds;
        WNDCLASSW wc{}; wc.lpfnWndProc = recorder_proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = RecorderClass; RegisterClassW(&wc);
        r.window = CreateWindowExW(0, RecorderClass, L"Frinky Clip recorder", 0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, &r);
        try { r.start(); }
        catch (const std::exception& e) { r.failure = e.what(); r.message = "Recorder could not start."; r.shutdown_core(); r.status(); throw; }
        if (!idle) r.resume();
        r.status();
        SetTimer(r.window, 1, 250, nullptr);
        MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        result = r.exit_code;
    } catch (const std::exception& e) {
        if (log) fprintf(log, "Frinky Clip: %s\n", e.what()); result = 1;
        auto d = read_json(app_dir() / "status.json"); obs_data_set_string(d.get(), "error", e.what()); obs_data_set_bool(d.get(), "recording", false);
        obs_data_set_string(d.get(), "state", "paused");
        obs_data_set_int(d.get(), "updated_ms", now_ms()); try { write_json(app_dir() / "status.json", d.get()); } catch (...) {}
    }
    base_set_log_handler(nullptr, nullptr); if (log) fclose(log);
    ReleaseMutex(mutex); CloseHandle(mutex); return result;
}
}
