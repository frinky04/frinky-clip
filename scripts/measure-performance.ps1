param(
    [ValidateRange(2, 86400)][int]$Seconds = 60,
    [ValidateRange(1, 60)][int]$IntervalSeconds = 2,
    [string]$Output = (Join-Path $PSScriptRoot "../test-output/performance.csv"),
    [string]$DataDirectory = $(if ($env:FRINKY_CLIP_HOME) { $env:FRINKY_CLIP_HOME } else { Join-Path $env:LOCALAPPDATA 'FrinkyClip' })
)
$ErrorActionPreference = 'Stop'
$samples = [System.Collections.Generic.List[object]]::new()
$previous = @{}
$clock = [Diagnostics.Stopwatch]::StartNew()
$previousTime = 0.0
do {
    # Raw counters avoid locale-specific counter names. Include the UI, recorder,
    # and their mux/export children; do not include unrelated OBS processes.
    $all = @(Get-CimInstance Win32_PerfRawData_PerfProc_Process)
    $selected = @{}
    foreach ($entry in $all) {
        if ($entry.Name -match '^frinky-clip(?:#\d+)?$') { $selected[[int]$entry.IDProcess] = $entry }
    }
    do {
        $added = $false
        foreach ($entry in $all) {
            if ($entry.IDProcess -and $selected.ContainsKey([int]$entry.CreatingProcessID) -and -not $selected.ContainsKey([int]$entry.IDProcess)) {
                $selected[[int]$entry.IDProcess] = $entry; $added = $true
            }
        }
    } while ($added)
    $now = $clock.Elapsed.TotalSeconds
    $cpuTicks = 0.0; $writeBytes = 0.0; $workingSet = 0.0; $privateBytes = 0.0
    foreach ($key in $selected.Keys) {
        $entry = $selected[$key]
        $workingSet += $entry.WorkingSet; $privateBytes += $entry.PrivateBytes
        if ($previous.ContainsKey($key) -and $previous[$key].ElapsedTime -eq $entry.ElapsedTime) {
            $cpuTicks += [Math]::Max(0, [double]$entry.PercentProcessorTime - [double]$previous[$key].PercentProcessorTime)
            $writeBytes += [Math]::Max(0, [double]$entry.IOWriteBytesPersec - [double]$previous[$key].IOWriteBytesPersec)
        }
    }
    $status = $null
    try { $status = Get-Content -LiteralPath (Join-Path $DataDirectory 'status.json') -Raw | ConvertFrom-Json } catch {}
    $fresh = $status -and ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - $status.updated_ms -lt 5000) -and $selected.ContainsKey([int]$status.pid)
    if ($previousTime -gt 0) {
        $elapsed = $now - $previousTime
        $samples.Add([pscustomobject]@{
            utc = [DateTimeOffset]::UtcNow.ToString('o')
            elapsed_seconds = [Math]::Round($now, 3)
            processes = $selected.Count
            cpu_machine_percent = [Math]::Round($cpuTicks / 1e7 / $elapsed / [Environment]::ProcessorCount * 100, 3)
            working_set_mib = [Math]::Round($workingSet / 1MB, 2)
            private_mib = [Math]::Round($privateBytes / 1MB, 2)
            process_write_mbps = [Math]::Round($writeBytes / 1e6 / $elapsed, 3)
            recording = [bool]($fresh -and $status.recording)
            recorder_fps = $(if ($fresh) { $status.fps } else { $null })
            render_ms = $(if ($fresh) { $status.render_ms } else { $null })
            render_frames = $(if ($fresh) { $status.render_frames } else { $null })
            render_lag_total = $(if ($fresh) { $status.lagged_frames } else { $null })
            encode_lag_total = $(if ($fresh) { $status.skipped_frames } else { $null })
            buffer_gb = $(if ($fresh) { $status.buffer_gb } else { $null })
        })
    }
    $previous = $selected; $previousTime = $now
    if ($now -ge $Seconds) { break }
    Start-Sleep -Seconds $IntervalSeconds
} while ($true)
$destination = [IO.Path]::GetFullPath($Output)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
$samples | Export-Csv -LiteralPath $destination -NoTypeInformation
$samples | Measure-Object -Property cpu_machine_percent, working_set_mib, private_mib, process_write_mbps -Average -Maximum |
    Select-Object Property, Average, Maximum
Write-Output "Saved $($samples.Count) samples to $destination"
