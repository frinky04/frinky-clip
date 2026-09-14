param([string]$ObsRoot = 'C:\Program Files\obs-studio', [string]$BuildDir = 'build', [switch]$Launch, [switch]$Package)
$ErrorActionPreference = 'Stop'
$clipTaskRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $clipTaskRoot
. "$PSScriptRoot/velopack.ps1"
$clipBuildPath = [IO.Path]::GetFullPath((Join-Path $clipTaskRoot $BuildDir))
if (-not $clipBuildPath.StartsWith($clipTaskRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'BuildDir must be inside this checkout.' }
$clipOutputPath = Join-Path $clipBuildPath 'bin/Release'
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
# A package is assembled from a fresh output folder so it holds exactly what the build stages.
if ($Package -and (Test-Path -LiteralPath $clipOutputPath)) { Remove-Item -LiteralPath $clipOutputPath -Recurse -Force }
& cmake -S . -B $clipBuildPath -G 'Visual Studio 18 2026' -A x64 "-DOBS_ROOT=$($ObsRoot.Replace('\','/'))"
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
& cmake --build $clipBuildPath --config Release --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }
& ctest --test-dir $clipBuildPath -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed.' }
if ($Package) {
  $version = (Select-String -LiteralPath 'CMakeLists.txt' -Pattern 'project\(FrinkyClip VERSION ([0-9.]+)').Matches[0].Groups[1].Value
  $name = "frinky-clip-$version-win64"
  $stage = Join-Path $clipTaskRoot "dist/$name"
  if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
  New-Item -ItemType Directory -Path $stage | Out-Null
  Copy-Item -Path "$clipOutputPath/*" -Destination $stage -Recurse
  Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -eq '.pdb' -or $_.Name -eq 'clip-tests.exe' } | Remove-Item -Force
  $zip = Join-Path $clipTaskRoot "dist/$name.zip"
  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  Compress-Archive -Path "$stage/*" -DestinationPath $zip
  $size = [math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)
  Write-Host "Packaged $zip ($size MB)"
}
if ($Launch) { Start-Process -FilePath "$clipOutputPath/frinky-clip.exe" -WindowStyle Hidden }
