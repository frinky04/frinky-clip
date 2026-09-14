param([string]$ObsRoot = 'C:\Program Files\obs-studio', [switch]$Launch)
$ErrorActionPreference = 'Stop'
$clipTaskRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $clipTaskRoot
if (-not (Test-Path -LiteralPath "$ObsRoot\bin\64bit\obs.dll")) { throw "Install OBS Studio 32.2.2, or pass -ObsRoot." }
$clipTaskVersion = (Get-Item -LiteralPath "$ObsRoot\bin\64bit\obs.dll").VersionInfo.ProductVersion
if ($clipTaskVersion -ne '32.2.2') { throw "This prototype targets OBS 32.2.2; found $clipTaskVersion. Update and verify the pinned headers before changing versions." }
$clipTaskRepos = @(
  @('obs-src', 'https://github.com/obsproject/obs-studio.git', '32.2.2'),
  @('imgui', 'https://github.com/ocornut/imgui.git', 'v1.92.5'),
  @('ffmpeg-src', 'https://github.com/FFmpeg/FFmpeg.git', 'n8.0')
)
foreach ($clipTaskRepo in $clipTaskRepos) {
  if (-not (Test-Path -LiteralPath ".deps/$($clipTaskRepo[0])/.git")) {
    & git clone --depth 1 --branch $clipTaskRepo[2] $clipTaskRepo[1] ".deps/$($clipTaskRepo[0])"
    if ($LASTEXITCODE) { throw 'Dependency checkout failed.' }
  }
}
& cmake -S . -B build -G 'Visual Studio 18 2026' -A x64 "-DOBS_ROOT=$($ObsRoot.Replace('\','/'))"
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
& cmake --build build --config Release --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }
& ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed.' }
if ($Launch) { Start-Process -FilePath "$clipTaskRoot\build\bin\Release\frinky-clip.exe" -WindowStyle Hidden }

