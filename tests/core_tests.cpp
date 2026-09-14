#include "buffer.hpp"
#include <shellapi.h>
#include <iostream>
#include <stdexcept>
using namespace clip;
static void require(bool pass, const char* text) { if (!pass) throw std::runtime_error(text); }
int main() {
    try {
        std::vector<Segment> segments{
            {"one", "session-a", 10000, 4, 100}, {"two", "session-a", 14000, 4, 100}, {"three", "session-a", 18000, 4, 100}};
        require(select_recent(segments, 5) == std::vector<size_t>({1, 2}), "Save covers requested duration at segment boundaries");
        require(select_recent(segments, 60).size() == 3, "Short buffer returns available history");
        require(expired_segments(segments, 18000, 100, 200, {}) == std::vector<size_t>({0}), "Byte limit evicts oldest first");
        require(expired_segments(segments, 18000, 5, 1000, {}) == std::vector<size_t>({0}), "Time retention expires old footage");
        require(expired_segments(segments, 18000, 100, 100, {fs::path("one")}) == std::vector<size_t>({1, 2}), "Pending save protects referenced footage");
        segments.push_back({"four", "session-b", 22000, 4, 100});
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
        std::cout << "Passed buffer retention, pinning, session boundaries, validation, Windows argument escaping, and atomic writes.\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
