#include "buffer.hpp"
#include "timeline.hpp"
#include "waveform.hpp"
#include <shellapi.h>
#include <iostream>
#include <stdexcept>
using namespace clip;
static void require(bool pass, const char* text) { if (!pass) throw std::runtime_error(text); }
int main() {
    try {
        std::vector<Segment> segments{
            {"one", "session-a", 6000, 10000, 100}, {"two", "session-a", 10000, 14000, 100}, {"three", "session-a", 14000, 18000, 100}};
        require(segments[0].seconds() == 4.0, "Segment length follows its chained extent");
        require(chained_ms(1000, 240, 60, 1) == 5000 && chained_ms(1000, 239, 60, 1) == 4983 && chained_ms(1000, 480, 60, 1) == 9000, "Chained segment times round once from the frame count");
        require(select_recent(segments, 5) == std::vector<size_t>({1, 2}), "Save covers requested duration at segment boundaries");
        require(select_recent(segments, 60).size() == 3, "Short buffer returns available history");
        require(expired_segments(segments, 18000, 100, 200, {}) == std::vector<size_t>({0}), "Byte limit evicts oldest first");
        require(expired_segments(segments, 18000, 5, 1000, {}) == std::vector<size_t>({0}), "Time retention expires old footage");
        require(expired_segments(segments, 18000, 100, 100, {fs::path("one")}) == std::vector<size_t>({1, 2}), "Pending save protects referenced footage");
        segments.push_back({"four", "session-b", 18000, 22000, 100});
        require(select_recent(segments, 60) == std::vector<size_t>({3}), "Never concatenate across encoder sessions");
        require(select_recent(segments, 60, "session-a").size() == 3, "Explicit session selection");
        require(select_recent({}, 60).empty(), "Empty buffer");
        Config c; c.storage = fs::temp_directory_path() / "FrinkyClip"; c.validate(); c.max_bitrate = 1;
        bool rejected = false; try { c.validate(); } catch (...) { rejected = true; } require(rejected, "Reject maximum bitrate below target");
        std::vector<std::wstring> args{L"plain", L"a b", L"C:\\dir with spaces\\", L"a\"b", L"", L"clip & $(echo no)"};
        std::wstring line = L"program"; for (auto& a : args) line += L" " + quote_arg(a);
        int argc; auto parsed = CommandLineToArgvW(line.c_str(), &argc); require(argc == (int)args.size() + 1, "Argument count round trips");
        for (size_t i = 0; i < args.size(); ++i) require(parsed[i+1] == args[i], "Windows argument escaping round trips"); LocalFree(parsed);
        auto dir = fs::temp_directory_path() / ("FrinkyClipTests-" + unique_id());
        auto file = dir / "state.json"; atomic_write(file, "first"); atomic_write(file, "second"); require(read_text(file) == "second", "Atomic replacement");
        require(!fs::exists(dir / "state.json.tmp"), "No temporary file after commit");
        wchar_t previous_home[32768]{};
        GetEnvironmentVariableW(L"FRINKY_CLIP_HOME", previous_home, 32768);
        require(SetEnvironmentVariableW(L"FRINKY_CLIP_HOME", dir.c_str()), "Isolate preference tests");
        auto saved = Config::load(); require(saved.record_on_launch, "Fresh install records on launch by default");
        saved.record_on_launch = false; saved.audio = false; saved.bitrate = 31000; saved.max_bitrate = 44000;
        saved.retention_minutes = 80; saved.budget_gb = 33.5; saved.save_seconds = 45; saved.share_bitrate = 12000;
        saved.hotkey = VK_F9; saved.modifiers = MOD_ALT; saved.monitor = "test display"; saved.storage = dir / L"Clips with spaces";
        saved.export_height = 720; saved.export_fps = 30;
        saved.save(); auto loaded = Config::load();
        require(!loaded.record_on_launch && !loaded.audio && loaded.bitrate == saved.bitrate && loaded.max_bitrate == saved.max_bitrate &&
            loaded.retention_minutes == saved.retention_minutes && loaded.budget_gb == saved.budget_gb && loaded.save_seconds == saved.save_seconds &&
            loaded.share_bitrate == saved.share_bitrate && loaded.hotkey == saved.hotkey && loaded.modifiers == saved.modifiers &&
            loaded.monitor == saved.monitor && loaded.storage == saved.storage && loaded.export_height == 720 && loaded.export_fps == 30,
            "All preferences survive a save/load round trip");
        saved.export_height = 900; rejected = false; try { saved.validate(); } catch (...) { rejected = true; } require(rejected, "Reject unsupported export height");
        saved.export_height = 720;
        auto valid_config = read_text(dir / "config.json"); saved.max_bitrate = 1;
        rejected = false; try { saved.save(); } catch (...) { rejected = true; }
        require(rejected && read_text(dir / "config.json") == valid_config, "Invalid draft cannot overwrite saved preferences");
        atomic_write(dir / "config.json", "{\"bitrate\":32000,\"audio\":false}");
        loaded = Config::load(); require(loaded.record_on_launch && loaded.bitrate == 32000 && !loaded.audio, "Old configs gain autostart without losing existing settings");
        loaded.record_on_launch = true; loaded.save(); require(Config::load().record_on_launch, "Enabled autostart persists");
        SetEnvironmentVariableW(L"FRINKY_CLIP_HOME", *previous_home ? previous_home : nullptr);
        fs::remove(dir / "config.json"); fs::remove(file); fs::remove(dir);
        std::vector<Span> spans{{"a1", "session-a", 1000, 5000}, {"a2", "session-a", 5000, 9000}, {"b1", "session-b", 20000, 24000}};
        require(spans_in_range(spans, 4000, 6000).size() == 2, "Range selects every overlapping segment");
        require(spans_in_range(spans, 5000, 5500).size() == 1 && spans_in_range(spans, 5000, 5500)[0].path == "a2", "Touching end excludes the earlier segment");
        require(spans_in_range(spans, 8000, 21000).empty(), "Range across sessions is rejected");
        require(spans_in_range(spans, 10000, 15000).empty(), "Range in a gap has no footage");
        ExportRequest request; request.start_ms = 3250; request.end_ms = 7000; request.height = 720; request.fps = 30; request.bitrate_kbps = 8000; request.codec = "av1"; request.mic = true;
        auto round_trip_dir = fs::temp_directory_path() / ("FrinkyClipExport-" + unique_id());
        write_export_request(round_trip_dir / "r.json", request); auto back = read_export_request(round_trip_dir / "r.json");
        require(back.start_ms == 3250 && back.end_ms == 7000 && back.height == 720 && back.fps == 30 && back.codec == "av1" && back.audio && back.mic, "Export request round trip");
        fs::remove_all(round_trip_dir);
        auto index_dir = fs::temp_directory_path() / ("FrinkyClipIndex-" + unique_id());
        BufferMap published; published.spans = {{"C:/b/session-a/s1.mkv", "session-a", 1000, 5000}, {"C:/b/session-a/s2.mkv", "session-a", 5000, 9000}}; published.last_end_ms = 9000;
        write_index(index_dir / "segments.json", published); auto loaded_index = read_index(index_dir / "segments.json");
        require(loaded_index && loaded_index->spans.size() == 2 && loaded_index->spans[1].start_ms == 5000 && loaded_index->last_end_ms == 9000 && loaded_index->spans[0].session == "session-a", "Segment index round trip");
        require(!read_index(index_dir / "missing.json"), "Missing index reports absence");
        fs::remove_all(index_dir);
        require(clip_name(0).starts_with("clip-19") && local_time(3600000 * 5 + 61234, true).ends_with(":01.234"), "Clip names and times use local wall-clock");
        wchar_t fixture[32768]{};
        if (GetEnvironmentVariableW(L"FRINKY_CLIP_TEST_SEGMENT", fixture, 32768)) {
            Config measured_config; measured_config.storage = fs::temp_directory_path() / ("FrinkyClipMeasure-" + unique_id());
            Buffer buffer(measured_config);
            auto segment = buffer.root() / "session-measure" / "segment-000000.mkv";
            fs::create_directories(segment.parent_path()); fs::copy_file(fs::path(fixture), segment);
            buffer.anchor("session-measure", 1000000);
            require(buffer.finalize(segment), "Closed segment is published before audio measurement");
            require(buffer.segments().size() == 1 && !buffer.segments()[0].measured && buffer.segments()[0].audio.empty(), "Finalization leaves audio pending");
            auto before = scan_buffer(buffer.root());
            require(before.spans.size() == 1 && before.spans[0].coarse.empty(), "Pending waveform does not hide closed footage");
            auto levels = audio_levels(segment); require(!levels.empty(), "Measurement fixture carries audio");
            buffer.set_levels(segment, std::move(levels));
            auto after = scan_buffer(buffer.root());
            require(buffer.segments()[0].measured && !after.spans[0].coarse.empty() && after.last_end_ms == before.last_end_ms, "Background measurement preserves the footage extent");
            {
                Waveforms waves; waves.retain({segment}); waves.levels(segment);
                waves.retain({}); waves.retain({segment});
                const std::vector<AudioLevels>* fine = nullptr;
                const auto deadline = steady_ms() + 2000;
                while (!fine && steady_ms() < deadline) { fine = waves.levels(segment); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
                require(fine && !fine->empty() && !fine->front().empty(), "Fine waveforms load after expiry and re-request");
                const auto revision = waves.revision();
                require(waves.levels(segment) == fine && waves.revision() == revision, "Published fine waveforms stay immutable without repeated loads");
            }
            fs::remove_all(measured_config.storage);
        }
        std::cout << "Passed buffer retention, pinning, session boundaries, validation, Windows argument escaping, and atomic writes.\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
