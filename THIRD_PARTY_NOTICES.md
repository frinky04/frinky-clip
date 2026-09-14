# Third-party components

Frinky Clip is distributed under GPL-2.0-or-later. The app packages the following upstream components; their own licences and notices continue to apply.

| Component | Source/build provenance | Licence |
| --- | --- | --- |
| OBS 32.2.2 runtime, plugins and helpers | https://github.com/obsproject/obs-studio/tree/32.2.2 | GPL-2.0-or-later; see upstream COPYING and component notices |
| FFmpeg 8.1.2 libraries and libx264 | Official OBS 32.2.2 Windows runtime; https://github.com/obsproject/obs-deps | GPL-enabled build; see SOURCES.md for source versions and patches |
| Remaining runtime DLLs (curl, RIST, SRT, pthreads, zlib) | Same OBS Windows runtime and dependency recipes | Individual upstream licences |
| Dear ImGui 1.92.5 | https://github.com/ocornut/imgui/tree/v1.92.5 | MIT |
| Velopack 1.2.0 | https://github.com/velopack/velopack/tree/1.2.0 | MIT; bundled third-party notices also apply |
| Noto Sans Mono Medium | https://github.com/notofonts/latin-greek-cyrillic | SIL Open Font License 1.1 |
| Microsoft Visual C++ runtime | Microsoft redistributable runtime, staged by CMake | Microsoft Visual Studio redistribution terms |

The installer and portable package include licence texts under `licenses`. Frinky Clip source and build scripts are at https://github.com/frinky04/frinky-clip. Release source material identifies the upstream source/build inputs for the bundled binaries; OBS is a build input and is not required on the recipient's computer.
