# Velopack implementation plan

Status: implementation and local installer/upgrade tests complete; GitHub publication and public-feed verification in progress.

## Outcome and scope

Distribute Frinky Clip for Windows x64 through a per-user installer and let installed copies discover, download, and apply updates from GitHub Releases. Ship unsigned initially. Existing users install once to move from the current ZIP to managed updates.

Use the existing `frinky04/frinky-clip` repository. The user has chosen to make it public for anonymous downloads. Making it public is part of implementation, not an action performed while writing this plan.

First release: `0.3.0`, with a separate `0.3.1` build used to prove upgrading. Keep signing, Windows sign-in startup, a website, additional update channels, and a broader first-run redesign outside this implementation. Keep the existing portable ZIP as a secondary download with manual updates.

Done means a clean Windows installation and a real GitHub-hosted upgrade both work, settings and recordings survive, and no recorder or updater process is stranded.

## Repository findings that determine the design

- `scripts/build.ps1` already builds, runs core tests, stages runtime files, and produces a ZIP. Reuse its staged folder.
- `cmake/stage.cmake` bundles OBS/FFmpeg/MSVC runtime dependencies. CI can extract the pinned OBS 32.2.2 Windows x64 ZIP instead of requiring an interactive OBS installation.
- `CMakeLists.txt` owns version `0.2.0`; retain it as the version authority.
- `src/main.cpp` dispatches GUI, recorder, and command-line modes through the same executable. Installer hooks must finish before that dispatch or any normal app initialization.
- `src/app.cpp` owns the recorder and its helpers in a Windows Job Object. The updater must run outside that job.
- Ordinary Quit currently terminates remaining workers after ten seconds. An update must not use that timeout to interrupt a long save/export.
- `src/ui.cpp` stops drawing when hidden. Update scheduling and completion handling must run before its hidden-window branch.
- `%LOCALAPPDATA%/FrinkyClip` contains persistent app data. The installation directory must be different so uninstall cannot remove that data.
- The repository contains untracked icon candidates in `assets/icons`. Coordinate with that work; do not replace or silently select those designs.

## 1. Establish packaging and startup integration

Pin Velopack's native SDK and `vpk` tool to the same version, initially **1.2.0**. Use a repository-local .NET tool manifest for `vpk`; download the native SDK to `.deps` with a pinned checksum. .NET is a packaging prerequisite, not an end-user prerequisite for this native application.

Use permanent package ID `Frinky04.FrinkyClip`, display name `Frinky Clip`, executable `frinky-clip.exe`, Windows x64, and one channel (`win`). Keep installation paths chosen by Velopack separate from `%LOCALAPPDATA%/FrinkyClip`.

Add the startup call before COM, graphics, OBS, single-instance handling, and command dispatch. Explicitly disable `SetAutoApplyOnStartup`: a recorder launch or a second invocation of the UI must never apply an update to the running installation. Installer lifecycle invocations must exit quickly with no UI, recording, or config writes.

Create the update manager only in the primary GUI process and only for installed builds. Development builds, standalone command modes, tests, and the existing portable ZIP must still run without update errors or automatic network checks.

Generate executable version/product metadata from CMake and use the same identity for package metadata. Integrate the chosen icon if the parallel icon work has supplied a final asset; icon design is not part of this task.

Initial proof: package two local versions, install the first, and exercise the SDK against a local feed. Verify startup hooks, the native SDK's download timeout/shutdown behavior, and the installed/development/portable distinction before adding UI or publication automation.

References: [native integration](https://docs.velopack.io/getting-started/cpp), [pinned C++ API](https://github.com/velopack/velopack/blob/1.2.0/src/lib-cpp/include/Velopack.hpp), [startup contract](https://docs.velopack.io/integrating/overview).

## 2. Add update checks and download UI

Introduce `src/updates.hpp` and `src/updates.cpp` as a small owner of the native update manager and background work. Use the SDK's `GithubSource` for `https://github.com/frinky04/frinky-clip`, without an access token and with prereleases excluded. Use the SDK's version comparison, package validation, download, and apply behavior; do not implement a second updater or GitHub release parser.

Keep UI-facing state simple: idle/current, checking, available, downloading, ready, or error. Run at most one network operation at a time. Transfer progress/results safely to the UI; background callbacks must not invoke ImGui or retain destroyed UI objects. Network activity must not block rendering, recording, or normal shutdown.

Add to Settings > App:

- Current version and `Check for updates`.
- `Automatically check for updates`, enabled by default, saved using the existing config mechanism.
- Available version with `Download update`, download progress, and `Update and restart` once ready.
- A release-notes link to the corresponding GitHub release.

Automatically check after startup settles and at most once per 24 hours while the tray app remains running. Persist the last attempted automatic check so repeated launches do not exhaust GitHub's anonymous API allowance. Manual checks bypass that interval but prevent concurrent requests and rapid repeated clicks. For the first version, downloading requires the user's click; checking alone never changes the installed version.

Offline, rate-limited, missing-feed, and download failures leave the installed version usable. Automatic failures go to diagnostics rather than modal dialogs; manual failures appear beside the update controls with Retry. Reuse the SDK's downloaded-pending-update information after restart, without automatically applying it.

Reference: [GitHub update source and rate limits](https://docs.velopack.io/integrating/update-sources).

## 3. Apply updates through a safe recorder shutdown

`Update and restart` requests a deliberate update shutdown:

1. Save valid pending preferences. If a draft setting is invalid, explain it and leave the app open.
2. Capture whether recording was running or paused, to restore that state after the update without changing `Record on launch`.
3. Stop accepting new start/save/export requests, including hotkeys and command-line messages. The recorder must enforce this itself, not just disabled UI controls.
4. Stop capture, close its current segment, and let accepted saves/exports drain. Display `Finishing save/export before updating` when necessary.
5. Wait until the recorder and helpers have exited. Use the existing ownership/cleanup machinery, but do not apply ordinary Quit's ten-second forced termination to this update path. If graceful shutdown fails, keep the update pending and expose the error; do not replace live binaries.
6. Arm the SDK's `WaitExitThenApplyUpdates` with restart enabled only after the recorder has drained. It waits up to 60 seconds for the UI process, so it must not be armed while waiting for an arbitrarily long export.
7. Release player/thumbnail resources and file handles, then return through the normal UI/process teardown. Do not copy the documentation's immediate `exit(0)` example into this application.
8. Restart exactly one tray app, restoring its pre-update recording state through a one-use restart argument.

If scheduling the updater fails, keep the application open in a safe stopped state, report the failure, and allow recording to resume. A new helper must not inherit the recorder's kill-on-close job. Normal Quit retains its existing semantics.

Also exercise installer repair/reinstallation and uninstall while the app is open. Stop the running tray owner and verify helper cleanup through the supported installer lifecycle; preserve configuration, clips, buffer, and pending recovery files.

## 4. Build reproducible release artifacts

Extend the current build/staging path rather than creating a separate copy of the dependency layout. Add `scripts/package.ps1` for Velopack packaging and a pinned native SDK bootstrap where the current dependency setup lives.

Produce:

- The generated `Frinky04.FrinkyClip-Setup.exe` as the primary download.
- Full update package and `releases.win.json`, plus other metadata required by `vpk upload`.
- The existing manually updated portable ZIP, avoiding a second ambiguous portable artifact.
- SHA-256 checksums, dependency notices, and the source/build material identified by the project's existing release terms. A public GitHub source archive alone should not be assumed to cover the bundled OBS/FFmpeg binaries.

Start with full update packages. Current ZIP size is about 24 MB, so delta generation is not needed to prove the experience and adds prior-release handling. Enable deltas later only if package sizes or update frequency justify them.

Exclude tests, benchmarks, PDBs, test outputs, and stale files from the package. Keep local developer builds working. Fail when tag, CMake version, executable metadata, and package version disagree. Never overwrite an already published version.

Reference: [Velopack distribution artifacts](https://docs.velopack.io/distributing/overview).

## 5. Publish with GitHub Actions

Add `.github/workflows/release.yml`, triggered by `v*` tags, with a manual build-only dispatch for checking the workflow before publication.

Pipeline:

1. Check out the tagged commit and verify its version.
2. Use a Windows x64 runner with the required Visual Studio 2026 C++ toolchain; explicitly check it instead of depending on an unverified `windows-latest` image. The runner-images repository currently documents a VS2026 Windows Server 2025 image; confirm its usable label during implementation.
3. Fetch checksum-pinned OBS 32.2.2 Windows x64 runtime ZIP, the matching headers, and pinned ImGui/FFmpeg/Velopack dependencies. Pass the extracted OBS path through existing `-ObsRoot` support.
4. Build Release, run core tests, and package the unsigned installer and updates. GPU-dependent capture tests remain a separate real-machine check.
5. Create a draft GitHub Release and upload all artifacts using the pinned `vpk` deployment tooling. Treat old ZIP-only releases as having no Velopack baseline; do not let an ordinary network/authentication failure masquerade as a first release.
6. Validate uploaded asset names, sizes, checksums, and the feed's referenced packages before publishing the release. Serialize release publication to avoid competing versions.
7. Publish the complete release. On failure, leave it as a draft so installed clients cannot see an incomplete update.

Use GitHub Actions' built-in `GITHUB_TOKEN` with `contents: write` for publication to this same repository. No token is bundled in the application. Normal development/PR builds get no release write permission. Signing credentials are not needed in this phase.

Make the existing repository public during implementation after reviewing the material that change exposes. Update README with installer/portable links, system requirements, update behavior, unsigned-build expectations, and a short release procedure. Do not add a second hosting service.

Reference: [Velopack GitHub Actions deployment](https://docs.velopack.io/distributing/github-actions).

## 6. Acceptance checks and first rollout

Automate the narrow lifecycle and package checks that protect recording or installation correctness; use real installed builds for the end-to-end proof.

| Scenario | Required result |
| --- | --- |
| Fresh Windows x64 installation without OBS/VS | Installer works as a normal user; app launches with bundled dependencies; Start menu and uninstall entries exist. Recording is verified separately on supported NVIDIA hardware. |
| Install/uninstall/update hooks | No unintended UI, recording, or second tray owner. |
| Upgrade A to B through a local feed, then real GitHub Releases | New version launches once; feed/package validation succeeds; old config and clips remain accessible. |
| Recording, paused, and long export during upgrade | Recording state restored; accepted media work finishes; no forced export truncation or orphan helpers. |
| Hidden-to-tray operation | Update checks complete without opening the editor or resuming rendering. |
| Offline, invalid package, missing asset, interrupted download, rate limit | Old app remains runnable; clear retry path; no apply of a partial/corrupt package. |
| Ordinary Quit during network work | Bounded shutdown; no updater-owned thread keeps the app alive indefinitely. |
| Launch app twice or invoke recorder/CLI with an update cached | No accidental update application, network check, or second primary owner. |
| Reinstall and uninstall, including while open | Processes close; installed binaries/shortcuts are removed as appropriate; user data remains. |
| Existing ZIP user | One manual installer run picks up their existing configuration and storage. |
| Unicode/spaced Windows username and storage path | Installation, upgrade, and restart work. |

Use isolated `FRINKY_CLIP_HOME` and storage directories, and a separate Windows account/VM for installation tests. Existing global mutex/window names mean the test copy must not run beside the user's real tray app. GPU recording tests require a supported real machine; a green hosted CI run alone is not sufficient evidence.

Publish `0.3.0` as the first installer only after local installation/upgrade checks pass. Exercise `0.3.0 -> 0.3.1` through the public GitHub feed, including downloading anonymously. Preserve immutable older artifacts for recovery. Fix a bad release by publishing a higher version containing the last known good code; do not introduce automatic downgrades in this phase.

Implementation order: startup/packaging proof, update checks/UI, safe update shutdown, GitHub release workflow, then clean-machine and public-feed validation. Stop when these acceptance checks pass.
