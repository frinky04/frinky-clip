# Frinky Clip: Graphite / Blue

Visual thesis: a compact graphite instrument panel with warm white text, thin dividers, square controls, and muted blue reserved for actions and focus.

Content plan: the clip editor is the work surface. A full-history overview bar sits above a zoomed lane view (ruler, video, system audio, microphone) with the clip range and export settings beneath it. The recorder lives in a footer strip: state, buffer clock, Record/Stop, quick save with its hotkey, and the clips folder. Capture, buffer, hotkey, app, and diagnostics live in a Settings overlay behind a single button. All controls remain native Dear ImGui widgets.

Clip editor concept: a clip is a range of the rolling buffer. Navigate with the overview (drag the viewport) and the lanes (drag to pan, scroll to zoom), mark in and out, and export a frame-accurate re-encode at the chosen resolution, frame rate, and bitrate to the clips folder. Lazy keyframe thumbnails and a preview strip with the exact In and Out frames stand in for a player; a keyframe stands in, dimmed, while the exact frame decodes. The video lane has a fixed height so thumbnails stay small and dense; spare height goes to the preview strip. Segments under the viewed or marked range are protected from expiry while the editor uses them. Exports run in the recorder process through ffmpeg.exe with NVENC, H.264 by default and AV1 as an option, with progress in the footer. Ruler labels are wall-clock time so a moment stays put while recording; time-ago appears only in the header. "Save last N seconds" is the separate quick path and keeps recording quality. Per-clip microphone selection waits on a separate microphone track.

Interaction thesis: immediate hover and keyboard-focus feedback; direct numeric editing and predictable Tab navigation; expand optional detail without transitions. No decorative motion or live preview: the utility should idle cheaply.

## Design decisions

- Use regular buttons throughout, including the footer. Avoid `SmallButton`; align adjacent text to the standard frame padding.

- Default to a 900 × 600 window, minimum 720 × 480, with the native title bar supplying the app name. The lanes have fixed heights and the preview strip takes the spare height; the footer keeps a fixed height including one message line.

- Use a two-column property table with stable widget IDs inside the Settings overlay rather than a stack of wide cards. Export resolution, frame rate, and bitrate sit on the editor's export row because they are chosen per clip.

- Put startup behavior in an App property row: label on the shared left edge, checkbox on the shared value edge. Keep it editable during recording and explain automatic saving in the tooltip.

- Use the supplied Noto Sans Mono Medium throughout the app at 16 px at 100% scaling, with a 19 px buffer clock. Bundle the font beside the executable; the native title bar retains the Windows system font.

- Keep backgrounds near black: #0B0C0E, #131518, #1B1E22. Primary text #E2E4E8, secondary text #9BA1AB, accent #7FA7CF.

- Keep controls compact, with 2 px rounding, 1 px field borders, and 3–7 px internal spacing. The native Windows title bar follows the dark palette.

- Use labels and status text as well as color. Enable keyboard navigation and maintain a visible focus highlight.

- Keep all actions, input fields, tooltips, and status output code-native. No image assets or new UI framework.

- Hide optional explanations in tooltips. Do not add marketing copy to this operating surface.

- Scale fonts and spacing together on DPI changes. Allow vertical scrolling on smaller windows.

## References

- [OpenAI frontend-skill, official pinned version](https://github.com/openai/skills/blob/82d2c5b44ac234ec0f204f647562a4349d90ef43/skills/.curated/frontend-skill/SKILL.md): app restraint, utility copy, one accent, and cardless composition. Installed unchanged into the user's Codex skills directory. The skill was removed from the main catalog in April 2026; this is its official historical version.

- [Dear ImGui getting started](https://github.com/ocornut/imgui/wiki/Getting-Started): standard Win32/DX11 integration and keyboard navigation.

- [Dear ImGui FAQ](https://github.com/ocornut/imgui/blob/master/docs/FAQ.md): stable IDs, DPI, fonts, and input handling.

- [Dear ImGui property editor example](https://github.com/ocornut/imgui/blob/v1.92.5/imgui_demo.cpp): table-based property layout and scoped IDs.

These are utility-app adaptations. The frontend skill's landing-page imagery and animation guidance does not apply to this recorder.


## App lifetime

The tray app owns recording and its helper processes. X hides the controls; Stop pauses recording while the recorder process stays warm and idle; Quit exits the whole app. The tray remains visible while paused, carries recording state in its tooltip, and restores itself after Explorer restarts. Auto-record applies only to a new tray-app lifetime. Hidden controls do not render.

Settings save automatically: checkbox/selection changes immediately, numeric and text edits on completion. Keep validation errors visible without overwriting the last valid configuration. No Apply button or routine saved-status label.

The app keeps no clip history and has no player; Folder opens the output location. Diagnostics lives at the bottom of Settings with logs and performance counters; errors and non-routine recorder messages show on the footer's message line. Omit explanatory text about standard window and tray behavior.
