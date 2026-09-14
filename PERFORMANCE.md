# Measuring recording impact

Measure the game's frame times with recording off and on. The recorder sustaining 60 fps does not establish that its impact on a game is small.

## Repeatable comparison

1. Use a game's built-in benchmark or a repeatable 2–3 minute route. Keep resolution, graphics settings, FPS cap, drivers, and other capture tools identical. Warm the game up first.
2. Use [NVIDIA FrameView](https://www.nvidia.com/en-gb/geforce/technologies/frameview/) or [Intel PresentMon](https://github.com/GameTechDev/PresentMon) to capture game frame times. FrameView can log FPS, frame times, and GPU power; PresentMon provides CPU/GPU/display timing. Choose one and use it consistently in every run.
3. Alternate recording off and on, at least three runs each. Close the Frinky Clip control window during the on runs to measure normal tray use. Do not save or export during the steady-state runs.
4. Compare median average FPS, 1% low FPS, and p95/p99 frame times across runs. Keep the same definition/tool for 1% lows. Average FPS loss is `100 × (off FPS − on FPS) / off FPS`. If the difference is within run-to-run noise, gather longer samples rather than claim zero impact.
5. Separately test a hotkey save and H.264 export during the same workload. Look for frame-time spikes and recorder lag. Export runs another encoder and has a different cost from ordinary recording.

At 60 fps, the recorder has a 16.67 ms frame interval. Expand Diagnostics to see cumulative render and encode misses for the last reported session. `status.json` also includes average OBS render time (`render_ms`), rendered/output frame totals, and buffer size. These are recorder diagnostics, not the game's frame times or full GPU utilization. Aim for no sustained growth in either lag counter after startup.

## Resource sampler

Run the opt-in sampler alongside each benchmark; use identical sampling in both conditions:

```powershell
./scripts/measure-performance.ps1 -Seconds 120 -Output test-output/off.csv
# Start recording, then repeat the same benchmark:
./scripts/measure-performance.ps1 -Seconds 120 -Output test-output/on.csv
```

The CSV reports combined Frinky Clip process CPU, working set, private memory, process write traffic, and recorder diagnostics every two seconds. It includes the UI, recorder, and their mux/export descendants. CPU is normalized to total machine capacity; 100% means all logical processors busy. Working sets can double-count shared pages. Write traffic includes pipes and other process I/O, so it is **not physical disk throughput**. Very short-lived child processes between samples may be missed. Missing/stale recorder data is left blank rather than reported as zero lag.

Sampling uses Windows performance counters and has its own overhead; it is only active when the script runs. Task Manager's GPU 3D/Copy/Video Encode engines and dedicated GPU memory can help diagnose contention; don't interpret the overall GPU percentage alone as recording overhead. FrameView's board-power logging is useful for comparing power under an identical workload.

## Longer checks before the next release

- Run for two hours, then check CPU/memory trends and buffer size against the quota. Old segments should expire without growing memory indefinitely.
- Save a long clip and check audio sync near the start, middle, and end; short smoke tests cannot establish long-session sync.
- In an isolated `FRINKY_CLIP_HOME`, interrupt a recorder and restart it. Check completed segments, protected pending jobs, and a decoded recovered clip. Keep the user's real recordings out of destructive tests.
- Repeat with the GPU close to full load. NVENC hardware encoding still involves capture, compositing, memory bandwidth, and disk work.

No controlled in-game A/B performance claim has been established yet. A synthetic recording or desktop-only sample verifies plumbing, not game performance.

## Editor and export checks

`--player-test <buffer> 2` exercises playback, range boundaries, superseding seeks,
audio failure fallback, thumbnail cancellation, retained textures, and cache budgets.
The audio failure check explicitly skips when no audio clock became active. It
injects the error state; it does not replace testing a physical endpoint change.

Build the optional real-window harness with
`cmake --build build --config Release --target editor-bench --parallel`, then run
`build/bin/Release/editor-bench.exe <segment.mkv> 6 12` and repeat with `1800`.
Use a segment with at least two seconds of video. This harness links the real
editor to a test-only app controller: it never starts recording, registers a tray
icon, or touches an existing recorder. It creates private fixture hardlinks and
metadata, exercises playback/range controls, resize, and hide/restore, and exits.
The 1,800-segment case models a two-hour index when using four-second footage;
it repeats the fixture's content and is not a two-hour recording soak test.
`test-output/editor-bench-last.txt` identifies the retained directory containing
`ui.csv`, `phases.csv`, and `result.txt` (or `error.txt`).

For a normal editor session, set `FRINKY_CLIP_UI_PERF` to an output CSV path before
launch. Samples include frame intervals, UI thread CPU time, Present duration,
picture timestamps, and thumbnail count. Logging is disabled by default and
buffered until normal exit. Separate steady playback from idle, seeking, resize,
and hidden intervals when comparing p95/p99; a hidden interval is deliberately
long. Also inspect memory and recording lag under the same workload.

`--export-test <buffer> 2` checks both codecs at 720p/30/60 and compares source-size
GPU input against forced CPU input at 60 fps. Every output is checked for frame
count, duration, dimensions, and audio presence. Confirm `GPU input active` before
attributing a result to the GPU path. Alternate several runs for throughput claims;
the CPU-input comparison still uses hardware decoding and NVENC. Downscaled
exports retain CPU scaling. Set `FRINKY_CLIP_TEST_SEGMENT` to an audio-bearing MKV
when running `ctest` to additionally verify publishing footage before waveform
measurement and adding the waveform without changing segment timing.

### Short validation on 2026-09-15

Release builds, the core tests with an audio fixture, and player checks on AV1
desktop footage, silent footage, and portrait H.264 footage passed. The real
editor harness completed with 6 and 1,800 segments, including resize/hide/restore.
In the 1,800-segment runs on an RTX 4080 SUPER, intervals with consecutive
advancing pictures had a baseline median/p95 of 16.50/31.87 ms and a final
median/p95 of 16.62/17.18 ms. These short scripted samples include transport
transitions and do not establish in-game performance or long-session stability.

For a two-second 1440p60 export across a seam, GPU versus forced CPU input took
1,156 versus 1,336 ms for H.264 and 1,032 versus 1,245 ms for AV1 (about 13% and
17% shorter). Both paths produced 120 frames with audio; all four outputs also
decoded completely without errors. These are single paired measurements, not
general speedup guarantees. Reports and baseline source snapshots are retained
under `test-output/perf-implementation`; editor CSV paths are recorded there.
