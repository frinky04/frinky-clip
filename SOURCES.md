# Release source material

Frinky Clip source, CMake configuration, dependency pins and packaging scripts are available at the Git tag matching each release. The Source code downloads on that release contain this material.

The bundled OBS/FFmpeg binaries are taken without modification from the official **OBS Studio 32.2.2 Windows x64** ZIP (SHA-256 `4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1`). Sources and build instructions for those binaries are available here:

- [OBS 32.2.2 complete release source archive](https://github.com/obsproject/obs-studio/releases/download/32.2.2/OBS-Studio-32.2.2-Sources.tar.gz), including upstream build instructions and component notices.
- [OBS dependency build recipes, patches and notices, 2026-07-15](https://github.com/obsproject/obs-deps/tree/8683107a02300923abe4f293920f4b5edc8cb624). This version is selected by OBS 32.2.2's CMakePresets.json. Every dependency recipe specifies its source repository, exact revision and build options.
- [Dependency recipes archive](https://github.com/obsproject/obs-deps/archive/8683107a02300923abe4f293920f4b5edc8cb624.tar.gz).
- [FFmpeg runtime source](https://github.com/FFmpeg/FFmpeg/archive/38b88335f99e76ed89ff3c93f877fdefce736c13.tar.gz), with the [OBS Windows patches and build options](https://github.com/obsproject/obs-deps/blob/8683107a02300923abe4f293920f4b5edc8cb624/deps.ffmpeg/99-ffmpeg.ps1). The runtime uses upstream 8.1.2; Frinky Clip compiles against the ABI-compatible 8.0 headers.
- [x264 runtime source](https://github.com/mirror/x264/archive/eaa68fad9e5d201d42fde51665f2d137ae96baf0.tar.gz), with the [OBS build instructions](https://github.com/obsproject/obs-deps/blob/8683107a02300923abe4f293920f4b5edc8cb624/deps.ffmpeg/40-x264.ps1).
- [Velopack 1.2.0 source](https://github.com/velopack/velopack/archive/refs/tags/1.2.0.tar.gz).
- [Dear ImGui 1.92.5 source](https://github.com/ocornut/imgui/archive/refs/tags/v1.92.5.tar.gz).

The installed `licenses` directory contains upstream licence texts, including the OBS dependency notices. Microsoft runtime DLLs are the redistributable runtime staged by CMake from Visual Studio; they are not part of the GPL source archives.
