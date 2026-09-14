param([Parameter(Mandatory)][string]$Version, [string]$Repository = 'frinky04/frinky-clip')
$ErrorActionPreference = 'Stop'
$clipPublishRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$clipReleaseDir = Join-Path $clipPublishRoot "dist/releases/$Version"
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Expected a stable semantic version.' }
$clipReleases = & gh api "repos/$Repository/releases?per_page=100"
if ($LASTEXITCODE) { throw 'Could not read releases. Refusing to publish.' }
if (($clipReleases | ConvertFrom-Json) | Where-Object tag_name -eq "v$Version") { throw 'This release tag already exists. Never overwrite a published release.' }
Set-Location -LiteralPath $clipPublishRoot
& dotnet tool run vpk -- upload github --repoUrl "https://github.com/$Repository" --outputDir $clipReleaseDir --channel win --tag "v$Version" --releaseName "Frinky Clip $Version" --skip-updates
if ($LASTEXITCODE) { throw 'Draft release upload failed.' }
# vpk uploads its own assets; add the portable ZIP and checksums explicitly.
& gh release upload "v$Version" --repo $Repository "$clipReleaseDir/frinky-clip-$Version-win64.zip" "$clipReleaseDir/SHA256SUMS.txt"
if ($LASTEXITCODE) { throw 'Additional release asset upload failed.' }
$clipRemote = & gh release view "v$Version" --repo $Repository --json isDraft,assets | ConvertFrom-Json
if ($LASTEXITCODE -or -not $clipRemote.isDraft) { throw 'Release must stay draft until validation completes.' }
$clipVerifyDir = Join-Path $clipPublishRoot "test-output/release-verify-$Version-$([Guid]::NewGuid().ToString('N'))"
& gh release download "v$Version" --repo $Repository --dir $clipVerifyDir
if ($LASTEXITCODE) { throw 'Could not verify uploaded release assets.' }
foreach ($clipLine in Get-Content "$clipReleaseDir/SHA256SUMS.txt") {
    $clipHash, $clipName = $clipLine -split '  ', 2
    # assets/RELEASES metadata is for tooling and need not be publicly shipped.
    if ($clipName -eq 'assets.win.json' -or $clipName -eq 'RELEASES') { continue }
    $clipDownloaded = Join-Path $clipVerifyDir $clipName
    if (-not (Test-Path $clipDownloaded) -or (Get-FileHash $clipDownloaded -Algorithm SHA256).Hash -ne $clipHash) { throw "Uploaded asset verification failed: $clipName" }
}
$clipNotes = @"
Install **Frinky04.FrinkyClip-win-Setup.exe** to get Frinky Clip and in-app updates. The portable ZIP is available for manual installation.

This build is unsigned, so Windows may show a SmartScreen warning. Requires Windows x64 and an NVIDIA GPU with H.264 NVENC encoding support. OBS does not need to be installed.

Settings and clips are preserved across updates and uninstall. Check for updates in Settings. Updates finish accepted saves and exports before restarting.
"@
$clipNotesFile = Join-Path $clipVerifyDir 'notes.md'
[IO.File]::WriteAllText($clipNotesFile, $clipNotes)
& gh release edit "v$Version" --repo $Repository --notes-file $clipNotesFile --draft=false --latest
if ($LASTEXITCODE) { throw 'Release publication failed.' }
