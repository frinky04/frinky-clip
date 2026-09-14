# Installed-app integration proof. Requires an interactive Windows desktop and
# supported NVIDIA GPU. Uses isolated media/config and refuses another app owner.
param([string]$FromVersion = '0.3.0', [string]$ToVersion = '0.3.1', [switch]$GitHub, [switch]$Paused, [switch]$LongExport)
$ErrorActionPreference = 'Stop'
$clipTestRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (Get-Process frinky-clip -ErrorAction SilentlyContinue) { throw 'Quit Frinky Clip before testing updates.' }
$clipRun = Join-Path $clipTestRoot "test-output/updates-$([Guid]::NewGuid().ToString('N'))"
$clipInstall = Join-Path $clipRun 'Installed App'
$clipHome = Join-Path $clipRun 'Preferences'
$clipStorage = Join-Path $clipRun 'Saved clips'
$clipFeed = Join-Path $clipRun 'feed'
New-Item -ItemType Directory -Force $clipHome,$clipStorage,$clipFeed | Out-Null
$clipOldHome = $env:FRINKY_CLIP_HOME; $clipOldFeed = $env:FRINKY_CLIP_UPDATE_FEED
$env:FRINKY_CLIP_HOME = $clipHome
$env:FRINKY_CLIP_UPDATE_FEED = if ($GitHub) { $null } else { $clipFeed }
$clipExe = Join-Path $clipInstall 'current/frinky-clip.exe'
function Read-Update { Get-Content "$clipHome/update-status.json" -Raw | ConvertFrom-Json }
function Read-Recorder { Get-Content "$clipHome/status.json" -Raw | ConvertFrom-Json }
function Wait-For([scriptblock]$Condition, [string]$Description, [int]$Seconds = 45) {
    $clipDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        try { if (& $Condition) { return } } catch {}
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $clipDeadline)
    throw "Timed out: $Description (test data: $clipRun)"
}
function Invoke-App([string]$Arguments) {
    $clipCommand = Start-Process -FilePath $clipExe -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    if (-not $clipCommand.WaitForExit(5000) -or $clipCommand.ExitCode -ne 0) { throw "App command failed: $Arguments" }
}
try {
    @{record_on_launch=$false;auto_check_updates=$false;storage=$clipStorage;save_seconds=5;export_height=720;export_codec='av1'} | ConvertTo-Json | Set-Content "$clipHome/config.json"
    [IO.File]::WriteAllText((Join-Path $clipStorage 'keep.txt'), 'User recordings must survive upgrades and uninstall.')
    if (-not $GitHub) { Copy-Item "$clipTestRoot/dist/releases/$ToVersion/*" $clipFeed }
    $clipSetup = Start-Process "$clipTestRoot/dist/releases/$FromVersion/Frinky04.FrinkyClip-win-Setup.exe" -ArgumentList @('--silent','--installto',('"'+$clipInstall+'"'),'--log',('"'+"$clipRun/install.log"+'"')) -WindowStyle Hidden -PassThru
    if (-not $clipSetup.WaitForExit(45000) -or $clipSetup.ExitCode -ne 0) { throw 'Installation failed.' }
    if (-not (Test-Path "$clipInstall/current/velopack_libc.dll")) { throw 'Native updater DLL is missing.' }
    $clipOwner = Start-Process $clipExe -WindowStyle Hidden -PassThru
    Wait-For { (Read-Update).current_version -eq $FromVersion -and (Read-Update).installed -and (Read-Recorder).state -eq 'paused' } 'installed app startup'
    Invoke-App '--update-test check'
    Wait-For { (Read-Update).available -and (Read-Update).version -eq $ToVersion } 'update discovery'
    if (-not $GitHub) {
        $clipPackage = (Get-Content "$clipFeed/releases.win.json" -Raw | ConvertFrom-Json).Assets | Where-Object Type -eq 'Full'
        [IO.File]::WriteAllText((Join-Path $clipFeed $clipPackage.FileName), 'Corrupt package')
        Invoke-App '--update-test download'
        Wait-For { -not (Read-Update).working -and (Read-Update).error } 'corrupt download rejection'
        if ((Read-Update).ready -or $clipOwner.HasExited) { throw 'Corrupt download affected the installed app.' }
        Copy-Item "$clipTestRoot/dist/releases/$ToVersion/$($clipPackage.FileName)" $clipFeed -Force
    }
    Invoke-App '--update-test download'
    Wait-For { (Read-Update).ready } 'update download' 60
    # Starting the executable again must not auto-apply the cached update.
    Invoke-App '--stop'
    Invoke-App '--updated-paused'
    if ((Read-Update).current_version -ne $FromVersion -or $clipOwner.HasExited) { throw 'Second launch auto-applied the update.' }
    Invoke-App '--start'
    Wait-For { (Read-Recorder).recording } 'recording before update'
    Start-Sleep -Seconds $(if ($LongExport) { 45 } else { 8 })
    Invoke-App '--save 5'
    if ($LongExport) {
        Wait-For { -not (Read-Recorder).busy } 'quick save before export'
        $clipSegments = (Get-Content "$clipHome/segments.json" -Raw | ConvertFrom-Json).segments
        $clipStart = $clipSegments[0].start_ms
        $clipEnd = $clipSegments[-1].end_ms
        Invoke-App "--export $clipStart $clipEnd"
        Wait-For { (Read-Recorder).busy -and (Read-Recorder).export_progress -ge 0 } 'long export start'
    }
    if ($Paused) {
        Invoke-App '--stop'
        Wait-For { (Read-Recorder).state -eq 'paused' } 'pause before update'
    }
    $clipBeforePid = (Read-Recorder).pid
    Invoke-App '--update-test apply'
    Wait-For { (Read-Update).current_version -eq $ToVersion -and (Read-Recorder).recording -eq (-not $Paused) -and (Read-Recorder).pid -ne $clipBeforePid } 'updated app restoring recording state' 60
    if (-not $clipOwner.WaitForExit(5000)) { throw 'Original owner survived update.' }
    if (Get-Process -Id $clipBeforePid -ErrorAction SilentlyContinue) { throw 'Old recorder survived update.' }
    if (-not (Test-Path "$clipStorage/keep.txt")) { throw 'Update deleted user storage.' }
    $clipSaved = @(Get-ChildItem "$clipStorage/clips" -Filter '*.mp4' -ErrorAction SilentlyContinue)
    if (-not $clipSaved.Count) { throw 'Accepted save did not finish before updating.' }
    if ($LongExport -and $clipSaved.Count -lt 2) { throw 'Long export was interrupted by update.' }
    $clipPrefs = Get-Content "$clipHome/config.json" -Raw | ConvertFrom-Json
    if ($clipPrefs.record_on_launch -or $clipPrefs.auto_check_updates -or $clipPrefs.storage.Replace('\','/') -ne $clipStorage.Replace('\','/')) { throw 'Upgrade changed saved preferences.' }
    # Every non-system media DLL must load from the installed app, not OBS.
    $clipAppProcess = Get-Process frinky-clip | Where-Object { $_.Path -eq $clipExe } | Select-Object -First 1
    $clipBadModules = @($clipAppProcess.Modules | Where-Object { $_.FileName -like '*obs-studio*' })
    if ($clipBadModules.Count) { throw 'App loaded runtime files from OBS installation.' }
    # Uninstall must also close an open tray app and its recorder/helpers.
    $clipUninstall = Start-Process "$clipInstall/Update.exe" -ArgumentList '--silent uninstall' -WindowStyle Hidden -PassThru
    if (-not $clipUninstall.WaitForExit(30000) -or $clipUninstall.ExitCode -ne 0) { throw 'Uninstall failed.' }
    Wait-For { -not (Get-Process frinky-clip -ErrorAction SilentlyContinue) } 'full shutdown on uninstall'
    if ((Test-Path $clipExe) -or -not (Test-Path "$clipHome/config.json") -or -not (Test-Path "$clipStorage/keep.txt")) { throw 'Uninstall did not preserve user data or remove the app.' }
    [IO.File]::WriteAllText((Join-Path $clipRun 'passed.txt'), "$FromVersion -> $ToVersion; installed, checked, downloaded, restored recording state, saved clip, preserved preferences, uninstalled. GitHub=$GitHub; Paused=$Paused; LongExport=$LongExport")
    Write-Host "Update integration passed: $clipRun"
} finally {
    # Close only this test's executable, never an unrelated Frinky Clip instance.
    if (Test-Path $clipExe) {
        $clipStillRunning = Get-Process frinky-clip -ErrorAction SilentlyContinue | Where-Object Path -eq $clipExe
        if ($clipStillRunning) { Invoke-App '--quit'; foreach ($clipProcess in $clipStillRunning) { [void]$clipProcess.WaitForExit(15000) } }
    }
    $env:FRINKY_CLIP_HOME = $clipOldHome; $env:FRINKY_CLIP_UPDATE_FEED = $clipOldFeed
}
