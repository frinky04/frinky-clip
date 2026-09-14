# Integration test: requires OBS/NVENC and a desktop session. Never runs against
# an existing Frinky Clip instance or the user's recording/config directory.
$ErrorActionPreference = 'Stop'
$exe = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../build/bin/Release/frinky-clip.exe'))
if (Get-Process -Name frinky-clip -ErrorAction SilentlyContinue) { throw 'Quit Frinky Clip before running lifecycle checks.' }
$previousHome = $env:FRINKY_CLIP_HOME
$testHome = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../test-output/lifecycle-$([Guid]::NewGuid().ToString('N'))"))
[IO.Directory]::CreateDirectory($testHome) | Out-Null
$env:FRINKY_CLIP_HOME = $testHome
$owner = $null
function Start-Owner { Start-Process -FilePath $exe -WindowStyle Hidden -PassThru }
function Command([string]$Arguments) {
    $commandProcess = Start-Process -FilePath $exe -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
    if ($commandProcess.ExitCode -ne 0) { throw "Command failed: $Arguments" }
}
function Wait-Recording {
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if ($owner.HasExited) { throw 'Tray app exited during startup.' }
        try {
            $state = Get-Content -LiteralPath (Join-Path $testHome 'status.json') -Raw | ConvertFrom-Json
            if ($state.recording -and (Get-Process -Id $state.pid -ErrorAction SilentlyContinue)) { return $state }
        } catch {}
        Start-Sleep -Milliseconds 200
    }
    throw 'Recorder did not start.'
}
function Wait-State([string]$Expected) {
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if ($owner.HasExited) { throw "Tray app exited while waiting for '$Expected'." }
        try {
            $state = Get-Content -LiteralPath (Join-Path $testHome 'status.json') -Raw | ConvertFrom-Json
            if ($state.state -eq $Expected -and (Get-Process -Id $state.pid -ErrorAction SilentlyContinue)) { return $state }
        } catch {}
        Start-Sleep -Milliseconds 200
    }
    throw "Recorder did not reach state '$Expected'."
}
function Children([int]$Parent) {
    $all = @(Get-CimInstance Win32_Process)
    $byId = @{}; foreach ($entry in $all) { $byId[[int]$entry.ProcessId] = $entry }
    $ids = [Collections.Generic.HashSet[int]]::new(); [void]$ids.Add($Parent)
    do {
        $added = $false
        foreach ($entry in $all) {
            # Parent IDs can refer to an exited process whose ID has been reused.
            if ($ids.Contains([int]$entry.ParentProcessId) -and $byId.ContainsKey([int]$entry.ParentProcessId) -and
                $entry.CreationDate -ge $byId[[int]$entry.ParentProcessId].CreationDate -and $ids.Add([int]$entry.ProcessId)) { $added = $true }
        }
    } while ($added)
    @($all | Where-Object { $ids.Contains([int]$_.ProcessId) -and $_.ProcessId -ne $Parent } |
        ForEach-Object { Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue })
}
function Assert-Exited($Processes) {
    foreach ($entry in $Processes) {
        if (-not $entry.WaitForExit(15000)) { throw "Process survived exit: $($entry.Id) $($entry.ProcessName)" }
    }
}
try {
    $owner = Start-Owner
    $state = Wait-Recording
    $worker = Get-Process -Id $state.pid
    Start-Sleep -Seconds 3
    if (-not $owner.CloseMainWindow()) { throw 'Could not send the title-bar close action.' }
    Start-Sleep -Milliseconds 700
    $owner.Refresh()
    if ($owner.HasExited -or $owner.MainWindowHandle -ne 0 -or $worker.HasExited) { throw 'X did not hide only the controls.' }
    $reopen = Start-Owner
    if (-not $reopen.WaitForExit(5000) -or $reopen.ExitCode -ne 0) { throw 'Reopen launched another app lifetime.' }
    $owner.Refresh()
    $after = Wait-Recording
    if ($owner.MainWindowHandle -eq 0 -or $after.pid -ne $state.pid) { throw 'Reopening changed the recording or did not show controls.' }
    $stopAt = Get-Date; Command '--stop'; $paused = Wait-State 'paused'
    $stopSeconds = ((Get-Date) - $stopAt).TotalSeconds
    if ($owner.HasExited) { throw 'Stop exited the tray app.' }
    if ($worker.HasExited -or $paused.pid -ne $state.pid) { throw 'Stop did not keep the recorder process warm.' }
    if ((Children $worker.Id).Count -ne 0) { throw 'Paused recorder left a mux helper running.' }
    [void]$owner.CloseMainWindow()
    $reopen = Start-Owner
    if (-not $reopen.WaitForExit(5000)) { throw 'Paused reopen did not reuse the app.' }
    Start-Sleep -Milliseconds 700
    $after = Get-Content -LiteralPath (Join-Path $testHome 'status.json') -Raw | ConvertFrom-Json
    if ($after.state -ne 'paused' -or $after.recording) { throw 'Reopening paused controls restarted recording.' }
    $startAt = Get-Date; Command '--start'; $resumed = Wait-Recording
    $startSeconds = ((Get-Date) - $startAt).TotalSeconds
    if ($resumed.pid -ne $state.pid) { throw 'Start after Stop launched a new recorder instead of resuming the warm one.' }
    Write-Output ("Warm stop took {0:N2}s, warm start took {1:N2}s (polled at 200 ms)." -f $stopSeconds, $startSeconds)
    Command '--stop'; $null = Wait-State 'paused'
    Command '--quit'; Assert-Exited @($owner, $worker)
    Write-Output 'PASS: X hides, reopen preserves session, Stop keeps a warm recorder, Start resumes it, paused reopen stays paused, Quit exits.'

    '{"record_on_launch":false}' | Set-Content -LiteralPath (Join-Path $testHome 'config.json')
    $owner = Start-Owner
    $idle = Wait-State 'paused'
    if ($owner.HasExited -or $idle.recording) { throw 'Disabled autostart did not keep an idle tray app.' }
    Command '--quit'; Assert-Exited @($owner)
    Write-Output 'PASS: disabled auto-record still runs the tray app with an idle recorder, and Quit fully exits.'

    '{"record_on_launch":true}' | Set-Content -LiteralPath (Join-Path $testHome 'config.json')
    $owner = Start-Owner; $state = Wait-Recording
    Start-Sleep -Seconds 10
    # Closed segments of a session must chain exactly, and an export across a
    # seam must come back frame-accurate through the in-process encoder.
    $index = Get-Content -LiteralPath (Join-Path $testHome 'segments.json') -Raw | ConvertFrom-Json
    if ($index.segments.Count -lt 2) { throw 'Expected at least two closed segments before exporting.' }
    $recordedCodec = & ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 $index.segments[-1].path
    if ($LASTEXITCODE -or $recordedCodec.Trim() -ne 'h264') { throw 'New recordings must use H.264.' }
    $chained = 0
    for ($i = 1; $i -lt $index.segments.Count; $i++) {
        if ($index.segments[$i].session -ne $index.segments[$i - 1].session) { continue }
        if ($index.segments[$i].start_ms -ne $index.segments[$i - 1].end_ms) { throw 'Segments of one session did not chain exactly.' }
        $chained++
    }
    if ($index.segments[-1].session -ne $index.segments[-2].session -or $chained -lt 1) { throw 'Expected two closed segments of the current session.' }
    $seam = [int64]$index.segments[-1].start_ms
    Command "--export $($seam - 1500) $($seam + 1500)"
    for ($attempt = 0; $attempt -lt 300; $attempt++) {
        Start-Sleep -Milliseconds 200
        $state = Get-Content -LiteralPath (Join-Path $testHome 'status.json') -Raw | ConvertFrom-Json
        if ($state.error) { throw "Export failed: $($state.error)" }
        if ($state.message -like 'Exported *') { break }
    }
    if ($state.message -notlike 'Exported *') { throw 'Export did not finish.' }
    $exported = & ffprobe -v error -select_streams v:0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 $state.last_clip
    if ($LASTEXITCODE -or [int]$exported -ne 180) { throw "Exported clip has $exported frames; expected 180." }
    $exportedCodec = & ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 $state.last_clip
    if ($LASTEXITCODE -or $exportedCodec.Trim() -ne 'h264') { throw 'Exports must use H.264.' }
    Write-Output 'PASS: segments chain exactly and a 3 s export across a seam holds exactly 180 frames.'
    $helpers = Children $owner.Id
    Command '--save 2'; Command '--quit'
    Assert-Exited (@($owner) + @($helpers))
    $state = Get-Content -LiteralPath (Join-Path $testHome 'status.json') -Raw | ConvertFrom-Json
    if ($state.recording -or -not $state.last_clip) { throw 'Quit did not finalize the pending save and stop recording.' }
    & ffmpeg -v error -xerror -i $state.last_clip -f null -
    if ($LASTEXITCODE) { throw 'Clip saved during Quit failed decoding.' }
    Write-Output 'PASS: Quit during a save finishes a decodable clip and leaves no recorder/mux helpers.'

    $owner = Start-Owner; $null = Wait-Recording
    $helpers = Children $owner.Id
    if ($helpers.Count -lt 2) { throw 'Expected recorder and mux helper for ownership check.' }
    $owner.Kill(); Assert-Exited (@($owner) + @($helpers))
    Write-Output 'PASS: forcibly terminating the tray app also terminates recorder and mux helper.'

    $unowned = Start-Process -FilePath $exe -ArgumentList '--recorder' -WindowStyle Hidden -Wait -PassThru
    if ($unowned.ExitCode -ne 1) { throw 'Unowned desktop recording was allowed.' }
    if (Get-Process -Name frinky-clip -ErrorAction SilentlyContinue) { throw 'A Frinky Clip process was left running.' }
    Write-Output "PASS: unowned recorder rejected. Artifacts: $testHome"
} finally {
    if ($owner -and -not $owner.HasExited) { $owner.Kill(); $owner.WaitForExit() }
    $env:FRINKY_CLIP_HOME = $previousHome
}
