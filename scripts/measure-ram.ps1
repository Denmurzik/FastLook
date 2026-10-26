#requires -Version 7
<#
.SYNOPSIS
  Memory acceptance (spec 9): Cold, Warm-idle, Warm-idle after use, 24 MP photo, plus the carried-over rows (after daemon trim,
  video residue, 24 MP with 24 MP neighbours + animation latency, hook callback right after the daemon's working-set trim).
.DESCRIPTION
  All values are Private Working Set, summed over FastLook.exe + FastLook.Viewer.exe (+ PreviewHost when present); median of -Runs.
    Cold            daemon only (prewarm off in a temp data dir), 12 s after start (after the idle trim; the 5 s value is kept as coldAt5sMB).
    WarmIdle        `FastLook.exe --warm`; sampled at 12 s (after the idle trim, like Cold) and at 35 s (after a heartbeat ping); the larger value counts. The prewarmed
                    Viewer must be running at both samples (else a note and warmViewerPresent=false); daemon and Viewer are reported separately.
    WarmAfterUse    warm + `--show photo24mp.jpg`, then `--toggle` the same file (hides it), 3 s later (decision 55: GPU device kept).
    WarmAfterTrim   the same, 12 s after the hide (the daemon trims 5 s after its last job).
    Photo           warm + `--show photo24mp.jpg`, 3 s later.
    VideoAfterClose warm + `--show video-1080p.mp4`, `--toggle` it away, 3 s and 12 s later (Viewer residue after a video).
    Neighbours      Viewer alone: 3 x 24 MP files shown with --preview + `next` (neighbours prefetch), Viewer + daemon baseline; also the
                    Viewer's anim_start / anim_in marks of the first show (while the neighbour decodes).
    HookAfterTrim   F15 taps (a harmless key) after the daemon trimmed itself; hook callback max / p99 from the daemon log.
  The --show/--toggle phases take the desktop (they open a window that grabs the foreground): the script holds
  %TEMP%\gui-e2e.lock (waits for it, deletes it in `finally`). Run it through the test lock. Writes out/perf/measure-ram.json.
#>
[CmdletBinding()]
param(
    [int]$Runs = 3,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$OutJson
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'lib\Win32.psm1') -Force
. (Join-Path $PSScriptRoot 'lib\GuiLock.ps1')

$bin = Join-Path $repo "out\bin\$Configuration"
$daemonExe = Join-Path $bin 'FastLook.exe'
foreach ($e in $daemonExe, (Join-Path $bin 'FastLook.Viewer.exe')) { if (-not (Test-Path $e)) { throw "Not built: $e (run scripts/build.ps1 first)" } }
if (-not $OutJson) { $OutJson = Join-Path $repo 'out\perf\measure-ram.json' }
elseif (-not [IO.Path]::IsPathRooted($OutJson)) { $OutJson = Join-Path $repo $OutJson }
New-Item -ItemType Directory -Force (Split-Path -Parent $OutJson) | Out-Null

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'make-perf-fixtures.ps1') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'make-perf-fixtures.ps1 failed' }
$fx = Join-Path $repo 'out\fixtures'
$photo = Join-Path $fx 'photo24mp.jpg'
$video = Join-Path $fx 'video-1080p.mp4'

$names = 'FastLook', 'FastLook.Viewer', 'FastLook.PreviewHost'
# Processes that existed before this script started are not ours: they are neither measured nor killed.
$script:preexisting = @(Get-Process -Name $names -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
function Get-Ours { @(Get-Process -Name $names -ErrorAction SilentlyContinue | Where-Object { $script:preexisting -notcontains $_.Id }) }
# Private Working Set, MB: daemon, Viewer and PreviewHost separately plus the sum.
function Get-Parts {
    $parts = [ordered]@{ daemon = 0.0; viewer = 0.0; preview = 0.0; viewerRunning = $false }
    foreach ($p in Get-Ours) {
        try { $mb = Get-PrivateWorkingSetMB -ProcessId $p.Id } catch { continue }
        switch ($p.ProcessName) {
            'FastLook' { $parts.daemon += $mb }
            'FastLook.Viewer' { $parts.viewer += $mb; $parts.viewerRunning = $true }
            default { $parts.preview += $mb }
        }
    }
    $parts.total = [math]::Round($parts.daemon + $parts.viewer + $parts.preview, 2)
    $parts.daemon = [math]::Round($parts.daemon, 2); $parts.viewer = [math]::Round($parts.viewer, 2)
    [pscustomobject]$parts
}
function Get-Total { (Get-Parts).total }
function Get-ProcMB([string]$Name) {
    $p = Get-Ours | Where-Object { $_.ProcessName -eq $Name } | Select-Object -First 1
    if ($p) { [math]::Round((Get-PrivateWorkingSetMB -ProcessId $p.Id), 2) } else { $null }
}
function Stop-All {
    if (Test-Path $daemonExe) { try { Start-Process $daemonExe -ArgumentList '--shutdown' -Wait -WindowStyle Hidden -ErrorAction SilentlyContinue } catch { } }
    Start-Sleep -Milliseconds 500
    Get-Ours | Stop-Process -Force -ErrorAction SilentlyContinue
}
function Get-Median([double[]]$v) {
    $v = @($v | Where-Object { $null -ne $_ })
    if ($v.Count -eq 0) { return $null }
    $s = $v | Sort-Object
    $n = $s.Count
    [math]::Round($(if ($n % 2) { $s[($n - 1) / 2] } else { ($s[$n / 2 - 1] + $s[$n / 2]) / 2 }), 2)
}

$savedData = $env:FASTLOOK_DATA_DIR
$savedPt = $env:FASTLOOK_POWERTOYS_DIR
$script:tmpDirs = @()
function New-Session([bool]$Prewarm) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("fastlook-ram-{0}" -f [guid]::NewGuid().ToString('N'))
    $data = Join-Path $tmp 'data'
    New-Item -ItemType Directory -Force $data, (Join-Path $tmp 'powertoys') | Out-Null
    $script:tmpDirs += $tmp
    $settings = @{ version = 1; general = @{ prewarm = $Prewarm; peekWarning = $false } }
    $settings | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $data 'settings.json') -Encoding utf8
    $env:FASTLOOK_DATA_DIR = $data
    $env:FASTLOOK_POWERTOYS_DIR = Join-Path $tmp 'powertoys'
    [pscustomobject]@{ Tmp = $tmp; Data = $data; Log = Join-Path $data 'logs\daemon.log' }
}
function Sleep-Until([Diagnostics.Stopwatch]$Watch, [int]$Ms) { $left = $Ms - $Watch.ElapsedMilliseconds; if ($left -gt 0) { Start-Sleep -Milliseconds $left } }
function Send-Cli([string[]]$CliArgs) { Start-Process $daemonExe -ArgumentList $CliArgs -Wait -WindowStyle Hidden | Out-Null }

$hookRe = 'hook installed hook_cb_max_us=(\d+) hook_cb_p50_us=(\d+) hook_cb_p99_us=(\d+) hook_cb_count=(\d+)'
function Read-HookStats([string]$Log) {
    $text = ''
    try { $fs = [IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite,Delete'); $sr = [IO.StreamReader]::new($fs); $text = $sr.ReadToEnd(); $sr.Dispose() } catch { }
    @([regex]::Matches($text, $hookRe) | ForEach-Object { [pscustomobject]@{ Max = [int]$_.Groups[1].Value; P50 = [int]$_.Groups[2].Value; P99 = [int]$_.Groups[3].Value; Count = [int]$_.Groups[4].Value } })
}
Add-Type -Namespace FastLookRam -Name Msg -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessageW(string name);
[DllImport("user32.dll")] public static extern bool PostMessageW(System.IntPtr hwnd, uint msg, System.IntPtr w, System.IntPtr l);
'@
function Flush-Hook([string]$Log) {
    $n = (Read-HookStats $Log).Count
    $m = [FastLookRam.Msg]::RegisterWindowMessageW('TaskbarCreated')
    [void][FastLookRam.Msg]::PostMessageW([IntPtr]0xFFFF, $m, [IntPtr]::Zero, [IntPtr]::Zero)
    $end = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $end) {
        $all = Read-HookStats $Log
        if ($all.Count -gt $n) { return $all[-1] }
        Start-Sleep -Milliseconds 100
    }
    $null
}

$cold = @(); $cold5 = @(); $coldDaemon = @(); $warm12 = @(); $warmDaemon = @(); $warmViewer = @(); $viewerMissing = $false; $warm = @(); $warm6 = @(); $warm35 = @(); $afterUse = @(); $afterTrim = @(); $photoMb = @()
$videoClose = @(); $videoMb = @(); $videoTrim = @(); $videoViewerTrim = @(); $hookTrim = @(); $notes = @()

try {
    Enter-GuiLock 'fastlook-measure-ram'
    if (Get-Process -Name FastLook -ErrorAction SilentlyContinue) { throw 'FastLook.exe is already running (the daemon mutex is per session); stop it first.' }

    for ($run = 1; $run -le $Runs; $run++) {
        Write-Host "--- run $run/$Runs ---"

        # Cold: daemon only, prewarm off.
        $s = New-Session $false
        $d = Start-Process $daemonExe -PassThru
        $csw = [Diagnostics.Stopwatch]::StartNew()
        Sleep-Until $csw 5000
        if ($d.HasExited) { throw "cold: daemon exited early ($($d.ExitCode))" }
        $cold5 += Get-Total
        # The daemon trims its working set 5 s after start/last job: Cold counts after that point, like Warm-idle does.
        Sleep-Until $csw 12000
        $cp = Get-Parts
        if ($cp.viewerRunning) { $notes += "run ${run}: a Viewer is running in the cold session (prewarm should be off)" }
        $cold += $cp.total; $coldDaemon += $cp.daemon
        Write-Host ("cold: {0} MB at 5 s, {1} MB at 12 s (after the idle trim)" -f $cold5[-1], $cold[-1])
        Stop-All

        # Warm-idle, then the photo / video phases in the same warm session.
        $s = New-Session $true
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $d = Start-Process $daemonExe -ArgumentList '--warm' -PassThru
        Sleep-Until $sw 6000
        if ($d.HasExited) { throw "warm: daemon exited early ($($d.ExitCode))" }
        $w6 = Get-Total
        Sleep-Until $sw 12000   # after the idle trim, same point as Cold
        $p12 = Get-Parts
        Sleep-Until $sw 35000   # after at least one heartbeat ping
        $p35 = Get-Parts
        # The prewarmed Viewer must exist (hidden, trimmed) at both samples, otherwise this is not a warm measurement.
        foreach ($pp in @(@('12 s', $p12), @('35 s', $p35))) {
            if (-not $pp[1].viewerRunning) { $notes += "run ${run}: no FastLook.Viewer process at $($pp[0]) of the warm session"; $viewerMissing = $true }
        }
        $vis = @(Find-Process-Windows -Name 'FastLook.Viewer' | Where-Object { $_.Visible }).Count
        if ($vis -gt 0) { $notes += "run ${run}: the prewarmed Viewer has a visible window in warm idle" }
        $warm6 += $w6; $warm12 += $p12.total; $warm35 += $p35.total; $warm += [math]::Max($p12.total, $p35.total)
        $warmDaemon += $p35.daemon; $warmViewer += $p35.viewer
        Write-Host ("warm-idle: {0} MB at 6 s, {1} MB at 12 s, {2} MB at 35 s; at 35 s daemon {3} + viewer {4} MB (viewer running: {5})" -f $w6, $p12.total, $p35.total, $p35.daemon, $p35.viewer, $p35.viewerRunning)

        Send-Cli @('--show', $photo)
        Start-Sleep -Seconds 3
        $visible = @(Find-Process-Windows -Name 'FastLook.Viewer' | Where-Object { $_.Visible }).Count -gt 0
        $photoMb += Get-Total
        if (-not $visible) { $notes += "run ${run}: no visible Viewer window 3 s after --show photo" }
        Write-Host ("photo: {0} MB (window visible: {1})" -f $photoMb[-1], $visible)

        Send-Cli @('--toggle', $photo)
        $hideAt = [Diagnostics.Stopwatch]::StartNew()
        Sleep-Until $hideAt 3000
        $afterUse += Get-Total
        Write-Host ("warm-idle after use: {0} MB" -f $afterUse[-1])
        $null = Flush-Hook $s.Log   # reset the histogram only after the sample (the rehook must not disturb it)
        Sleep-Until $hideAt 8000   # the daemon trimmed itself 5 s after its last job
        # Hook callback right after the trim: three F15 taps (harmless key), then flush the histogram through a rehook.
        if ($d.HasExited) { throw 'daemon exited during the after-use phase' }
        $null = Flush-Hook $s.Log
        Start-Sleep -Milliseconds 400
        $tapped = 0
        for ($i = 0; $i -lt 3; $i++) { Send-Key -Vk 0x7E; $tapped++; Start-Sleep -Milliseconds 120 }
        $h = Flush-Hook $s.Log
        if ($h) { $hookTrim += $h; Write-Host ("hook after trim: n={0} p50={1} p99={2} max={3} us" -f $h.Count, $h.P50, $h.P99, $h.Max) }
        else { $notes += "run ${run}: hook stats not flushed after trim" }
        Sleep-Until $hideAt 12000
        $afterTrim += Get-Total
        Write-Host ("warm-idle after trim: {0} MB" -f $afterTrim[-1])

        # Video residue.
        Send-Cli @('--show', $video)
        Start-Sleep -Seconds 4
        $vVisible = @(Find-Process-Windows -Name 'FastLook.Viewer' | Where-Object { $_.Visible }).Count -gt 0
        $videoMb += Get-Total
        if (-not $vVisible) { $notes += "run ${run}: no visible Viewer window 4 s after --show video" }
        if ($videoMb.Count -and $afterTrim.Count -and $videoMb[-1] - $afterTrim[-1] -lt 5) { $notes += "run ${run}: the video added < 5 MB while shown; it may not have played" }
        Send-Cli @('--toggle', $video)
        $vh = [Diagnostics.Stopwatch]::StartNew()
        Sleep-Until $vh 3000
        $videoClose += Get-Total
        Sleep-Until $vh 12000
        $videoTrim += Get-Total
        $videoViewerTrim += (Get-ProcMB 'FastLook.Viewer')
        Write-Host ("video: {0} MB 3 s after close, {1} MB after trim (viewer alone {2} MB)" -f $videoClose[-1], $videoTrim[-1], $videoViewerTrim[-1])
        Stop-All
    }

    # 24 MP image with two 24 MP neighbours: Viewer alone via --preview (first show prefetches the neighbour, `next` makes both
    # sides neighbours). Memory counts the Viewer plus the median cold daemon.
    Write-Host '--- 24 MP with 24 MP neighbours ---'
    $neigh = @(); $animStart = @(); $animIn = @(); $shotsDir = Join-Path $repo 'out\shots'
    $files = @($photo, (Join-Path $fx 'photo24mp-2.jpg'), (Join-Path $fx 'photo24mp-3.jpg'))
    for ($run = 1; $run -le $Runs; $run++) {
        # Array parameters do not bind through `pwsh -File`: build the call as a command.
        $fileList = ($files | ForEach-Object { "'$_'" }) -join ','
        $call = "& '{0}' -Files @({1}) -WaitMs 6000 -ExtraArgs @('--script','wait:2500;next;wait:2000') -Out '{2}'" -f (Join-Path $PSScriptRoot 'run-preview.ps1'), $fileList, (Join-Path $shotsDir 'perf-neighbours.png')
        $out = & pwsh -NoProfile -Command $call 2>&1
        $res = @($out | Where-Object { "$_" -match '^RESULT ' }) | Select-Object -Last 1
        if ($res -and "$res" -match 'privateWS_MB=(\S+)') { $neigh += [double]::Parse($Matches[1].Replace(',', '.'), [Globalization.CultureInfo]::InvariantCulture) }
        foreach ($l in $out) {
            if ("$l" -match '^anim_start .* ms=([\d.,]+)') { $animStart += [double]::Parse($Matches[1].Replace(',', '.'), [Globalization.CultureInfo]::InvariantCulture) }
            elseif ("$l" -match '^anim_in .* ms=([\d.,]+)') { $animIn += [double]::Parse($Matches[1].Replace(',', '.'), [Globalization.CultureInfo]::InvariantCulture) }
        }
        Write-Host ("neighbours: viewer {0} MB, anim_start {1}, anim_in {2}" -f $neigh[-1], ($animStart -join '/'), ($animIn -join '/'))
    }
}
finally {
    Stop-All
    if ($null -ne $savedData) { $env:FASTLOOK_DATA_DIR = $savedData } else { Remove-Item Env:\FASTLOOK_DATA_DIR -ErrorAction SilentlyContinue }
    if ($null -ne $savedPt) { $env:FASTLOOK_POWERTOYS_DIR = $savedPt } else { Remove-Item Env:\FASTLOOK_POWERTOYS_DIR -ErrorAction SilentlyContinue }
    foreach ($t in $script:tmpDirs) { Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue }
    Exit-GuiLock
}

$coldMed = Get-Median $cold
$summary = [ordered]@{
    runs                  = $Runs
    coldMB                = $coldMed
    warmIdleMB            = Get-Median $warm
    coldAt5sMB            = Get-Median $cold5
    coldDaemonMB          = Get-Median $coldDaemon
    warmViewerPresent     = -not $viewerMissing
    warmDaemonMB          = Get-Median $warmDaemon
    warmViewerMB          = Get-Median $warmViewer
    warmIdleAt12sMB       = Get-Median $warm12
    warmIdleAt6sMB        = Get-Median $warm6
    warmIdleAt35sMB       = Get-Median $warm35
    warmAfterUseMB        = Get-Median $afterUse
    warmAfterTrimMB       = Get-Median $afterTrim
    photoMB               = Get-Median $photoMb
    videoShownMB          = Get-Median $videoMb
    videoAfterCloseMB     = Get-Median $videoClose
    videoAfterTrimMB      = Get-Median $videoTrim
    videoViewerAfterTrimMB = Get-Median $videoViewerTrim
    neighboursViewerMB    = Get-Median $neigh
    neighboursTotalMB     = if ($coldMed -and $neigh.Count) { [math]::Round((Get-Median $neigh) + $coldMed, 2) } else { $null }
    animStartMs           = [ordered]@{ median = Get-Median $animStart; max = ($animStart | Measure-Object -Maximum).Maximum; values = $animStart }
    animInMs              = [ordered]@{ median = Get-Median $animIn; max = ($animIn | Measure-Object -Maximum).Maximum; values = $animIn }
    hookAfterTrimUs       = [ordered]@{ max = ($hookTrim | Measure-Object -Property Max -Maximum).Maximum; p99 = ($hookTrim | Measure-Object -Property P99 -Maximum).Maximum; n = ($hookTrim | Measure-Object -Property Count -Sum).Sum }
    perRun                = [ordered]@{ cold = $cold; warm6 = $warm6; warm35 = $warm35; afterUse = $afterUse; afterTrim = $afterTrim; photo = $photoMb; videoClose = $videoClose; videoTrim = $videoTrim; neighboursViewer = $neigh }
    notes                 = $notes
}
$summary | ConvertTo-Json -Depth 6 | Set-Content $OutJson -Encoding utf8

Write-Host ''
Write-Host ("FastLook Private Working Set, {0}, median of {1} runs (daemon + Viewer summed)" -f $Configuration, $Runs)
[pscustomobject]@{
    Cold = $summary.coldMB; WarmIdle = $summary.warmIdleMB; AfterUse = $summary.warmAfterUseMB; AfterTrim = $summary.warmAfterTrimMB
    Photo = $summary.photoMB; VideoClose = $summary.videoAfterCloseMB; VideoTrim = $summary.videoAfterTrimMB; Neighbours = $summary.neighboursTotalMB
} | Format-Table -AutoSize | Out-String | Write-Host
$notes | ForEach-Object { Write-Host "note: $_" }
Write-Host "JSON: $OutJson"
