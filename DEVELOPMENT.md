# Development

## Build

Requires Windows x64, Visual Studio 2026 C++ tools, CMake, Git, and OBS Studio **32.2.2** as a build input.

```powershell
./scripts/build.ps1
```

The script fetches pinned headers and ImGui/Velopack dependencies, builds Release, runs core tests, and stages the OBS/FFmpeg/MSVC runtime beside the executable. OBS is not an end-user prerequisite. `-BuildDir build-release` uses a separate output directory; `-ObsRoot` accepts an extracted official OBS ZIP.

To obtain the checksum-verified CI runtime input:

```powershell
./scripts/prepare-obs.ps1
./scripts/build.ps1 -ObsRoot "$pwd/.deps/obs-runtime-32.2.2"
```

## Package and publish

Install the .NET 10 SDK on the build machine for the pinned Velopack tool:

```powershell
./scripts/package.ps1
```

Outputs go to `dist/releases/<version>`: unsigned installer, full update package, `releases.win.json`, checksums, and portable ZIP. `scripts/build.ps1 -Package` produces only the portable ZIP.

`CMakeLists.txt` owns the executable and package version. Commit, tag `v<version>`, then push the commit and tag. `.github/workflows/release.yml` builds and tests on Windows with VS2026, uploads a draft GitHub Release, verifies its assets by downloading them, and publishes. Tag/version mismatches fail. Existing releases are not overwritten. Manual workflow dispatch builds artifacts without publishing.

The installed app uses Velopack's native GitHub source without authentication. Only the primary tray process checks/downloads updates. Automatic apply-on-startup is disabled because the same executable also runs recorder and CLI modes. Update shutdown drains accepted media work before handing off to the updater; ordinary Quit retains its ten-second limit. Full update packages are used initially. Signing is not yet configured.

## Test

```powershell
ctest --test-dir build -C Release --output-on-failure
./scripts/test-lifecycle.ps1
./scripts/test-updates.ps1 -FromVersion 0.3.0 -ToVersion 0.3.1
./scripts/test-updates.ps1 -FromVersion 0.3.0 -ToVersion 0.3.1 -Paused -LongExport
```

Package both versions before running update tests. Add `-GitHub` to test against the public release feed instead of local packages. These tests use isolated settings/storage and require an interactive desktop with supported NVIDIA hardware. Quit any running copy first: the app's global instance/window identities are shared. Hosted CI cannot verify GPU recording or replace clean-machine testing.

`FRINKY_CLIP_HOME` overrides `%LOCALAPPDATA%/FrinkyClip` for isolated tests. With that override set, `FRINKY_CLIP_UPDATE_FEED` accepts a local test feed, and `--update-test check|download|apply` invokes the same actions as Settings. Neither facility is used for normal installations.

## Diagnostics and architecture

Settings and logs live in `%LOCALAPPDATA%/FrinkyClip`. Capture diagnostics are in `recorder.log`, crashes in `crash.log`, and update failures in `update-error.txt`. Buffer, clips and pending saves live under the configured storage folder.

The tray app owns its recorder/helpers in a Windows Job Object. Stop keeps the recorder warm; Quit closes the whole process group. Hidden controls do not render. Settings and storage remain outside the installation directory.

See [DESIGN.md](DESIGN.md) for architecture, [PERFORMANCE.md](PERFORMANCE.md) for benchmarks and long-session checks, and [SOURCES.md](SOURCES.md) for bundled dependency source/build material. HDR, Windows sign-in launch and app-specific audio exclusion remain future work.
