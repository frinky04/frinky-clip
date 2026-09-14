#pragma once
#include "common.hpp"
namespace clip {
int run_recorder(bool synthetic, int seconds, bool idle);
int run_ui(int resume_recording = -1);
int player_test(const fs::path& buffer_root, int play_seconds); // Headless decode benchmark.
}
