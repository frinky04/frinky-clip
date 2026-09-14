param([string]$ObsRoot = 'C:\Program Files\obs-studio', [string]$BuildDir = 'build', [string]$ExpectedVersion = '')
$ErrorActionPreference = 'Stop'
$clipPackageRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $clipPackageRoot
$clipVersion = (Select-String CMakeLists.txt -Pattern 'project\(FrinkyClip VERSION ([0-9.]+)').Matches[0].Groups[1].Value
if ($ExpectedVersion -and $ExpectedVersion -ne $clipVersion) { throw "Tag version $ExpectedVersion differs from CMake version $clipVersion." }
& "$PSScriptRoot/build.ps1" -ObsRoot $ObsRoot -BuildDir $BuildDir -Package
& dotnet tool restore
if ($LASTEXITCODE) { throw 'Could not restore the pinned Velopack tool.' }
$clipStage = Join-Path $clipPackageRoot "dist/frinky-clip-$clipVersion-win64"
$clipPackageOut = Join-Path $clipPackageRoot "dist/releases/$clipVersion"
if ((Get-Item "$clipStage/frinky-clip.exe").VersionInfo.ProductVersion -ne $clipVersion) { throw 'Executable version differs from the package.' }
if (-not (Test-Path "$clipStage/velopack_libc.dll")) { throw 'Missing velopack_libc.dll (the SDK import name differs from its download filename).' }
# Each version is packed into an empty directory, avoiding stale feed entries.
if (Test-Path $clipPackageOut) {
    $clipResolved = (Resolve-Path -LiteralPath $clipPackageOut).Path
    $clipAllowed = [IO.Path]::GetFullPath((Join-Path $clipPackageRoot 'dist/releases')) + [IO.Path]::DirectorySeparatorChar
    if (-not $clipResolved.StartsWith($clipAllowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe package directory.' }
    Remove-Item -LiteralPath $clipResolved -Recurse -Force
}
New-Item -ItemType Directory -Force $clipPackageOut | Out-Null
Copy-Item LICENSE,THIRD_PARTY_NOTICES.md,SOURCES.md -Destination $clipStage
Copy-Item licenses -Destination $clipStage -Recurse
& dotnet tool run vpk -- pack --packId Frinky04.FrinkyClip --packTitle 'Frinky Clip' --packAuthors Frinky04 --packVersion $clipVersion --mainExe frinky-clip.exe --packDir $clipStage --outputDir $clipPackageOut --runtime win-x64 --channel win --delta None --noPortable --shortcuts StartMenuRoot --skip-updates
if ($LASTEXITCODE) { throw 'Velopack packaging failed.' }
# Refresh the portable ZIP with the same notices as the installer.
$clipPortable = Join-Path $clipPackageOut "frinky-clip-$clipVersion-win64.zip"
Compress-Archive -Path "$clipStage/*" -DestinationPath $clipPortable
$clipFeed = Get-Content "$clipPackageOut/releases.win.json" -Raw | ConvertFrom-Json
$clipFull = @($clipFeed.Assets | Where-Object Type -eq 'Full')
if ($clipFull.Count -ne 1 -or $clipFull[0].Version -ne $clipVersion -or $clipFull[0].PackageId -ne 'Frinky04.FrinkyClip') { throw 'Unexpected release feed identity.' }
foreach ($clipEntry in $clipFeed.Assets) {
    $clipFile = Join-Path $clipPackageOut $clipEntry.FileName
    if (-not (Test-Path $clipFile) -or (Get-Item $clipFile).Length -ne $clipEntry.Size) { throw 'Release feed references a missing or incomplete package.' }
}
$clipHashes = Get-ChildItem $clipPackageOut -File | Sort-Object Name | ForEach-Object { "$( (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $($_.Name)" }
[IO.File]::WriteAllLines((Join-Path $clipPackageOut 'SHA256SUMS.txt'), $clipHashes)
Write-Host "Installer and update feed: $clipPackageOut"
