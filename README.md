# Frinky Clip

Windows tray recorder: 1440p60 SDR display capture, NVENC AV1, a rolling disk buffer, and a global save hotkey. The controls use Dear ImGui with the bundled Noto Sans Mono Medium font.

## Use

Run `build/bin/Release/frinky-clip.exe`. Recording starts automatically. Uncheck **Record on launch** to disable this; the toggle saves immediately and can be changed during recording. It controls app launch, not Windows sign-in. Auto-record runs once when the tray app starts. Reopening its controls, including by launching the executable again, does not restart a paused recording.

- Default save hotkey: **Ctrl+Shift+F8**, preserving the last 60 seconds plus segment-boundary overlap.
- **X** hides the controls to the tray. The tray app stays open whether recording or paused. Its menu offers Open, Start/Stop Recording, Save Clip, and **Quit**.
- **Stop** stops capture but keeps the tray app and its recorder process open. The recorder stays warm between sessions (OBS core, graphics device, and plugins loaded, capture source and encoders released), so Record and Stop take well under a second; only app launch pays the full OBS start-up. Startup also trusts committed segment metadata instead of re-probing every buffered file, so it no longer slows down with a large buffer. **Quit** stops recording and exits the app and its worker processes. Quit allows up to 10 seconds for the current segment/save to finish; longer work is interrupted with source clips and pending footage retained.
- Hidden controls do not render. The recorder and mux/export helpers belong to the tray app through a Windows Job Object, so an unexpected app exit also terminates them. No service or independent recorder remains after Quit.
- `frinky-clip.exe --quit` invokes the same full exit as the tray menu. `--stop` only stops recording and `--start` resumes it. Direct desktop `--recorder` invocation is internal and requires ownership by the tray app; isolated synthetic tests are the exception.
- The window is the clip editor over the rolling buffer. The overview bar shows the whole history with footage, the marked range, and the viewed window; drag it to move. The lanes show a wall-clock ruler, a filmstrip of keyframe thumbnails tiled across each run of footage, and audio coverage. Click to place the playhead, left-drag to mark a range, drag a handle to adjust it, drag the ruler to scrub, scroll to zoom, right-drag to pan, and **Now** returns to the live edge. Handles snap to source frames. The viewport above the lanes plays the buffer from the playhead with desktop audio: **Play**, Space, or a click on the picture toggles playback, I and O mark the range at the playhead, and the arrow keys step one frame (one second with Shift). Playback stops at the last closed segment. Footage in view or marked stays protected from expiry while the window is open.
- **Export** re-encodes the range through `ffmpeg.exe` (on PATH for now) with NVENC at the chosen resolution, frame rate, codec (H.264 default, AV1 optional), and bitrate, into the clips folder as `clip-YYYYMMDD-HHMMSS.mp4`. The cut is frame-accurate. Progress shows in the footer; the editor stays usable. A range must lie within one recording session and end in closed footage. **Save last N seconds** remains the separate quick path at recording quality.
- The recorder footer holds the state, buffer clock, Record/Stop, Save last N seconds with its hotkey, and the clips folder. **Settings** opens capture, buffer, hotkey, app, and diagnostics.
- Stop recording to edit capture settings. Checkboxes and selections save immediately; numeric fields and Storage save when you press Enter or leave the field. Hiding or quitting also saves valid pending edits. Invalid settings show a Not saved message and leave the last valid config intact.
- **Diagnostics** at the bottom of Settings has Open Log and recorder performance counters. The log records core start-up time and how long each session took to start and stop. The recorder announces every status change to the control window and reacts to OBS callbacks immediately, so state, save, and export updates appear within a frame rather than after a poll.
- Preferences live in `%LOCALAPPDATA%/FrinkyClip/config.json`; status and logs are alongside them. `FRINKY_CLIP_HOME` overrides this directory for isolated tests.
- Buffer, clips, and pending saves live under the configured Storage folder. The disk budget covers the rolling buffer; saved clips, pending jobs, and quarantined interrupted files are outside it.
- The export row's resolution, frame rate, codec, and bitrate are saved as defaults. The earlier H.264 share export of the last saved clip is no longer on the surface; `frinky-clip.exe --share` still triggers it. `--probe-frame <segment.mkv> <offset_ms>` writes decode timings to `frame-probe.txt` for diagnosing thumbnails.

The default history is 120 minutes with a 50 GB limit. At a sustained 40 Mbps, two hours of video is about 36 GB, plus audio and container overhead. Actual AV1 VBR size depends on content. Footage expires at whichever limit is reached first.

## Build

Requires Windows x64, Visual Studio 2026 C++ tools, CMake, Git, and OBS Studio **32.2.2** installed. Run:

```powershell
./scripts/build.ps1
```

The script fetches pinned OBS, FFmpeg, and ImGui headers/source. It builds Release and runs the core tests. Keep the runtime DLLs, `runtime.json`, and font alongside the executable. This is not yet a standalone distributable: OBS plugins and data still load from the configured OBS installation. H.264 share export additionally needs FFmpeg on PATH.

## Reliability and current limits

The ring uses short MKV segments. Finished clips are remuxed to a temporary MP4, checked, flushed, and renamed into place. Pending saves protect their source segments and are retried after restart. Recovery retains unusable tails as `.interrupted`; it is not a guarantee against filesystem or hardware failure.

Audio waveforms, a separate microphone track, stitching several ranges, a bundled ffmpeg.exe, app-specific audio exclusion, HDR, Windows sign-in launch, and export while the recorder process is not running are future work. Thumbnails, seeking, and playback decode AV1 on the GPU through D3D11VA, with the native FFmpeg `av1` decoder selected explicitly because the default AV1 decoder in the bundled build is software-only. Playback frames stay in video memory and are converted from NV12 by a shader. Measured on an RTX 4080: about 1 ms per frame, a keyframe thumbnail in under 10 ms, a seek to an exact frame in about 100 ms, resume after pause in under 20 ms, and a forward frame step in a few milliseconds. Segment files are opened without a stream probe, seeks within the open segment reuse it, and the recorder publishes `segments.json` so the editor reads one file instead of every sidecar. If the device cannot decode, everything falls back to software. Playback is paced by the audio device; without audio it uses the wall clock. `frinky-clip.exe --player-test <buffer folder> [seconds]` runs a headless benchmark of these paths and writes `player-test.txt`. Capture and encoding remain fixed at 1440p60. The latest clip remains accessible across recorder restarts while its file and status entry still exist.

Before treating this as a dependable daily recorder, complete a two-hour run under gaming load and verify A/V sync across long saves, disk-limit eviction, and interruption recovery. See [PERFORMANCE.md](PERFORMANCE.md) for the benchmark procedure.

## Checks completed

`scripts/test-lifecycle.ps1` exercises the tray app in isolated storage. Quit any existing app before running it. It checks X/hide and reopen, Stop keeping a warm recorder that Start resumes in the same process, paused reopen without auto-record, disabled auto-record, Quit during a save with clip decoding, full helper cleanup, and forced owner termination. These lifecycle checks passed; direct unowned desktop recording is rejected.

The cleanup pass verified preference round trips and old-config defaults, invalid settings leaving saved preferences intact, startup enabled/disabled across launches, changing the startup toggle while recording, latest-clip retention across recorder restarts, a decoded saved clip, and graceful recorder shutdown. A forced recorder-process interruption in isolated storage recovered two decodable segments. The resource sampler produced a valid CSV during desktop capture. These short tests do not replace the long-session and in-game checks above.
