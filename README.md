# Frinky Clip

Windows tray recorder: 1440p60 SDR display capture, NVENC AV1, a rolling disk buffer, and a global save hotkey. The controls use Dear ImGui with the bundled Noto Sans Mono Medium font.

## Use

Run `build/bin/Release/frinky-clip.exe`. Recording starts automatically. Uncheck **Record on launch** to disable this; the toggle saves immediately and can be changed during recording. It controls app launch, not Windows sign-in. Auto-record runs once when the tray app starts. Reopening its controls, including by launching the executable again, does not restart a paused recording.

- Default save hotkey: **Ctrl+Shift+F8**, preserving the last 60 seconds plus segment-boundary overlap.
- **X** hides the controls to the tray. The tray app stays open whether recording or paused. Its menu offers Open, Start/Stop Recording, Save Clip, and **Quit**.
- **Stop** stops capture but keeps the tray app and its recorder process open. The recorder stays warm between sessions (OBS core, graphics device, and plugins loaded, capture source and encoders released), so Record and Stop take well under a second; only app launch pays the full OBS start-up. Startup also trusts committed segment metadata instead of re-probing every buffered file, so it no longer slows down with a large buffer. **Quit** stops recording and exits the app and its worker processes. Quit allows up to 10 seconds for the current segment/save to finish; longer work is interrupted with source clips and pending footage retained.
- Hidden controls do not render. The recorder and mux/export helpers belong to the tray app through a Windows Job Object, so an unexpected app exit also terminates them. No service or independent recorder remains after Quit.
- `frinky-clip.exe --quit` invokes the same full exit as the tray menu. `--stop` only stops recording and `--start` resumes it. Direct desktop `--recorder` invocation is internal and requires ownership by the tray app; isolated synthetic tests are the exception.
- The window is the clip editor: an overview of the whole buffer, zoomable lanes (drag to pan, scroll to zoom), the clip range, and export settings. Range marking, thumbnails, and frame-accurate export are the next phase; the lanes are empty until then. The recorder footer holds the state, buffer clock, Record/Stop, Save last N seconds with its hotkey, and the clips folder. **Settings** opens capture, buffer, hotkey, app, and diagnostics.
- Stop recording to edit capture settings. Checkboxes and selections save immediately; numeric fields and Storage save when you press Enter or leave the field. Hiding or quitting also saves valid pending edits. Invalid settings show a Not saved message and leave the last valid config intact.
- **Diagnostics** at the bottom of Settings has Open Log and recorder performance counters. The log records core start-up time and how long each session took to start and stop.
- Preferences live in `%LOCALAPPDATA%/FrinkyClip/config.json`; status and logs are alongside them. `FRINKY_CLIP_HOME` overrides this directory for isolated tests.
- Buffer, clips, and pending saves live under the configured Storage folder. The disk budget covers the rolling buffer; saved clips, pending jobs, and quarantined interrupted files are outside it.
- The export row's resolution, frame rate, and bitrate are saved as defaults for the clip editor. The earlier H.264 share export of the last saved clip is no longer on the surface; `frinky-clip.exe --share` still triggers it and needs `ffmpeg.exe` on PATH.

The default history is 120 minutes with a 50 GB limit. At a sustained 40 Mbps, two hours of video is about 36 GB, plus audio and container overhead. Actual AV1 VBR size depends on content. Footage expires at whichever limit is reached first.

## Build

Requires Windows x64, Visual Studio 2026 C++ tools, CMake, Git, and OBS Studio **32.2.2** installed. Run:

```powershell
./scripts/build.ps1
```

The script fetches pinned OBS, FFmpeg, and ImGui headers/source. It builds Release and runs the core tests. Keep the runtime DLLs, `runtime.json`, and font alongside the executable. This is not yet a standalone distributable: OBS plugins and data still load from the configured OBS installation. H.264 share export additionally needs FFmpeg on PATH.

## Reliability and current limits

The ring uses short MKV segments. Finished clips are remuxed to a temporary MP4, checked, flushed, and renamed into place. Pending saves protect their source segments and are retried after restart. Recovery retains unusable tails as `.interrupted`; it is not a guarantee against filesystem or hardware failure.

The editor/timeline, exact trimming, app-specific audio exclusion, HDR, Windows sign-in launch, and independent export while the recorder is stopped are future work. Capture and encoding remain fixed at 1440p60. The latest clip remains accessible across recorder restarts while its file and status entry still exist.

Before treating this as a dependable daily recorder, complete a two-hour run under gaming load and verify A/V sync across long saves, disk-limit eviction, and interruption recovery. See [PERFORMANCE.md](PERFORMANCE.md) for the benchmark procedure.

## Checks completed

`scripts/test-lifecycle.ps1` exercises the tray app in isolated storage. Quit any existing app before running it. It checks X/hide and reopen, Stop keeping a warm recorder that Start resumes in the same process, paused reopen without auto-record, disabled auto-record, Quit during a save with clip decoding, full helper cleanup, and forced owner termination. These lifecycle checks passed; direct unowned desktop recording is rejected.

The cleanup pass verified preference round trips and old-config defaults, invalid settings leaving saved preferences intact, startup enabled/disabled across launches, changing the startup toggle while recording, latest-clip retention across recorder restarts, a decoded saved clip, and graceful recorder shutdown. A forced recorder-process interruption in isolated storage recovered two decodable segments. The resource sampler produced a valid CSV during desktop capture. These short tests do not replace the long-session and in-game checks above.
