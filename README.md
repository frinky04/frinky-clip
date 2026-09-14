# Frinky Clip

A Windows tray app that keeps your recent gameplay ready to save. Capture at 1440p60, save a clip with a hotkey, or trim a moment in the built-in editor.

**[Download for Windows](https://github.com/frinky04/frinky-clip/releases/latest/download/Frinky04.FrinkyClip-win-Setup.exe)** · [All releases](https://github.com/frinky04/frinky-clip/releases) · [Report a bug](https://github.com/frinky04/frinky-clip/issues)

## Requirements

- 64-bit Windows and an NVIDIA GPU with AV1 encoding support.
- A current NVIDIA driver. OBS does not need to be installed.

## Install

Run the installer, then open **Frinky Clip** from the Start menu. A portable ZIP is also available on the releases page.

Releases are currently unsigned, so Windows may show a SmartScreen warning. After checking that the download came from this repository, choose **More info → Run anyway**.

## Use

- Recording starts when you open the app. Change **Record on launch** in Settings to disable this.
- Press **Ctrl+Shift+F8** to save the last 60 seconds.
- Use the timeline to select a range, then **Export**. **Space** plays or pauses; **I** and **O** mark the start and end.
- Desktop audio and your default microphone are recorded. Change either in Settings.
- After your first launch, Frinky Clip starts quietly in the tray when you sign in. Turn off **Start with Windows** in Settings to disable this.
- Closing the window keeps the app in the tray. Choose **Quit** from the tray menu to exit.

By default, Frinky Clip keeps up to two hours of history within a 50 GB buffer limit. Change the storage folder and limits in Settings. Saved clips are kept separately from the rolling buffer.

## Updates

Installed copies check for updates automatically. Open **Settings → Updates** to download and restart when ready. Updates and uninstall preserve your settings and clips. Portable copies are updated manually.

[GPL-2.0-or-later](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md)
