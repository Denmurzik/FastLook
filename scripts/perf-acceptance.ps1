#requires -Version 7
<#
.SYNOPSIS
  Performance acceptance (spec 9): runs every measurement and prints a PASS/FAIL table (spec rows + carried-over rows).
.DESCRIPTION
  Steps, in order: smoke (smoke.ps1), ram (measure-ram.ps1), bench (bench.ps1, mixed fixtures, -BenchIterations), video (bench.ps1
  over a video-only folder: Private Bytes over repeated video opens), e2e (e2e-hook.ps1 -Iterations N), cold (e2e-hook.ps1 -Cold).
  Each step's console output goes to out/perf/<step>.log, its JSON to out/perf/<step>.json; the rows and all values go to
  out/perf-acceptance.json, rewritten after EVERY step, so a killed run still leaves the finished rows behind.

  Bench memory rows (task 34, lead ruling): the acceptance row is the growth of FastLook's own memory (heap busy bytes, summed
  over the 50-open intervals without a graphics-driver pool step) with the window really closing between opens; the graphics driver's pool (D3DKMT)
  is reported in an INFO row. INFO rows never fail the run.

  Honesty rules: before a step runs its JSON is deleted; a step that throws, exits non-zero (ram, bench, video) or leaves no JSON
  gives "not measured" (= FAIL), never an earlier run's numbers. Steps not selected (-Only / -Skip) read the JSON stored by an
  earlier run on purpose (that is how a run in parts is assembled). Each step waits for >= 2 GB of free RAM (up to 10 min), else
  it is not run and its rows say "not measured". Exit code 1 when any row is FAIL; MANUAL rows never fail the run.

  Run in parts on a loaded machine: -Only smoke,ram  then  -Only bench,video  then  -Only e2e  then  -Only cold.
  WARNING: ram and e2e/cold take the desktop and inject keys (e2e ~10-15 min at 1000 iterations); smoke and bench show real
  windows. Do not touch keyboard or mouse. Each step takes %TEMP%\gui-e2e.lock itself (released between steps). Run through the
  test lock. Pass lists with -Command (`pwsh -Command "& scripts/perf-acceptance.ps1 -Only smoke,ram"`); with -File write
  `-Only smoke,ram` as one comma-separated value, it is split here.
#>
[CmdletBinding()]
param(
    [int]$BenchIterations = 1000,
    [int]$E2EIterations = 1000,
    [int]$VideoIterations = 200,
    [string[]]$Only = @(),
    [string[]]$Skip = @(),
    [int]$MinFreeRamMB = 2000,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$perf = Join-Path $repo 'out\perf'
New-Item -ItemType Directory -Force $perf | Out-Null

$allSteps = 'smoke', 'ram', 'bench', 'video', 'e2e', 'cold'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Skip = @($Skip | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($n in $Only + $Skip) { if ($allSteps -notcontains $n) { throw "unknown step '$n' (steps: $($allSteps -join ', '))" } }
$selected = @($allSteps | Where-Object { ($Only.Count -eq 0 -or $Only -contains $_) -and $Skip -notcontains $_ })

$cfg = @('-Configuration', $Configuration)
$json = @{
    smoke = Join-Path $perf 'smoke.json'; ram = Join-Path $perf 'measure-ram.json'; bench = Join-Path $perf 'bench.json'
    video = Join-Path $perf 'bench-video.json'; e2e = Join-Path $perf 'e2e-warm.json'; cold = Join-Path $perf 'e2e-cold.json'
}
$steps = [ordered]@{
    smoke = @{ script = 'smoke.ps1'; args = $cfg + @('-OutJson', $json.smoke) }
    ram   = @{ script = 'measure-ram.ps1'; args = $cfg + @('-OutJson', $json.ram) }
    bench = @{ script = 'bench.ps1'; args = $cfg + @('-Iterations', $BenchIterations, '-OutJson', $json.bench) }
    video = @{ script = 'bench.ps1'; args = $cfg + @('-Iterations', $VideoIterations, '-Dir', (Join-Path $repo 'out\bench-video'), '-OutJson', $json.video) }
    e2e   = @{ script = 'e2e-hook.ps1'; args = $cfg + @('-Iterations', $E2EIterations, '-OutJson', $json.e2e) }
    cold  = @{ script = 'e2e-hook.ps1'; args = $cfg + @('-Cold', '-OutJson', $json.cold) }
}
# Steps whose non-zero exit means the measurement itself is invalid. smoke/e2e/cold exit 1 on their own check failures; their JSON
# is still a valid measurement and the rows judge the values.
$strictExit = 'ram', 'bench', 'video'

$state = @{}   # name -> @{ ran; exit; note }

function Get-FreeRamMB { [int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1KB) }
function Wait-FreeRam {
    $end = [DateTime]::UtcNow.AddMinutes(10)
    while ((Get-FreeRamMB) -lt $MinFreeRamMB) {
        if ([DateTime]::UtcNow -gt $end) { return $false }
        Write-Host ("   free RAM {0} MB < {1} MB, waiting" -f (Get-FreeRamMB), $MinFreeRamMB)
        Start-Sleep -Seconds 30
    }
    $true
}

# Returns the parsed JSON of a step, or $null when the step ran and failed / left nothing. A step that did not run reads the stored file.
function Get-StepData([string]$Name) {
    $st = $state[$Name]
    if ($st -and $st.ran) {
        if ($null -eq $st.exit) { return $null }
        if ($strictExit -contains $Name -and $st.exit -ne 0) { return $null }
    }
    if (-not (Test-Path $json[$Name])) { return $null }
    try { Get-Content $json[$Name] -Raw | ConvertFrom-Json } catch { $null }
}
function Get-ExitText([string]$Name) {
    $st = $state[$Name]
    if ($st -and $st.ran) { if ($null -eq $st.exit) { 'no exit code' } else { "$($st.exit)" } } else { 'stored' }
}
function Num($x) { if ($null -eq $x -or "$x" -eq '') { $null } else { [double]$x } }
function Growth($d) {
    if ($d -and $null -ne $d.privateBytesEndMB -and $null -ne $d.privateBytesAt100MB) { [math]::Round([double]$d.privateBytesEndMB - [double]$d.privateBytesAt100MB, 2) } else { $null }
}
# Bench memory split (lead ruling, task 34): "own" = heap busy bytes (what FastLook allocates); "driver" = the graphics kernel's
# count of this process's GPU allocations in system memory (D3DKMT), which the Intel user-mode driver backs with private memory and
# keeps in pools that grow in steps. The private commit outside the heaps moves with the driver and is reported. The driver also
# keeps bookkeeping in the process heap that grows with each pool step, so own growth is summed only over the 50-open intervals
# without a driver step (> 1 MB); the raw growth and the growth inside step intervals are reported. $null when the bench JSON has
# no split (older Viewer) or a sample is missing.
function Split-Growth($d) {
    if (-not $d -or $null -eq $d.series -or $null -eq $d.ownAt100MB -or $null -eq $d.ownEndMB -or $null -eq $d.driverEndMB) { return $null }
    $pts = @($d.series | Where-Object { $_.i -ge 100 } | Sort-Object i)
    if ($pts.Count -eq 0) { return $null }
    $pts += [pscustomobject]@{ i = [int]$d.iterations + 1; ownMB = [double]$d.ownEndMB; driverMB = [double]$d.driverEndMB }
    $own = 0.0; $ownInSteps = 0.0; $stepMB = 0.0; $steps = 0; $driverRest = 0.0
    for ($k = 1; $k -lt $pts.Count; $k++) {
        $dOwn = [double]$pts[$k].ownMB - [double]$pts[$k - 1].ownMB
        $dDrv = [double]$pts[$k].driverMB - [double]$pts[$k - 1].driverMB
        if ([math]::Abs($dDrv) -gt 1) { $ownInSteps += $dOwn; $stepMB += $dDrv; $steps++ } else { $own += $dOwn; $driverRest += $dDrv }
    }
    [pscustomobject]@{
        own = [math]::Round($own, 2); ownInSteps = [math]::Round($ownInSteps, 2)
        driver = [math]::Round([double]$d.driverEndMB - [double]$d.driverAt100MB, 2); driverSteps = $steps; driverStepMB = [math]::Round($stepMB, 2)
        driverRest = [math]::Round($driverRest, 2); handles = [int]$d.handlesEnd - [int]$d.handlesAt100; privateBytes = Growth $d
        ownRaw = [math]::Round([double]$d.ownEndMB - [double]$d.ownAt100MB, 2); intervals = $pts.Count - 1
        nonHeap = if ($null -ne $d.nonHeapEndMB -and $null -ne $d.nonHeapAt100MB) { [math]::Round([double]$d.nonHeapEndMB - [double]$d.nonHeapAt100MB, 2) } else { $null }
    }
}

function Build-Rows {
    $smoke = Get-StepData 'smoke'; $ram = Get-StepData 'ram'; $bench = Get-StepData 'bench'
    $video = Get-StepData 'video'; $e2e = Get-StepData 'e2e'; $cold = Get-StepData 'cold'
    $rows = [System.Collections.Generic.List[object]]::new()
    # $Value: number/text or $null (not measured). $Test gets the value. $Manual rows never fail.
    function Add-Row([string]$Metric, $Value, [string]$Target, [scriptblock]$Test, [string]$Source, [switch]$Manual, [switch]$Info, [string]$Detail = '') {
        $status = 'FAIL'; $shown = 'not measured'
        if ($Manual) { $status = 'MANUAL'; $shown = 'release checklist' }
        elseif ($Info) { $status = 'INFO'; if ($null -ne $Value -and "$Value" -ne '') { $shown = if ($Value -is [double]) { '{0:n1}' -f $Value } else { "$Value" } } }
        elseif ($null -ne $Value -and "$Value" -ne '') {
            $shown = if ($Value -is [double] -or $Value -is [float]) { '{0:n1}' -f $Value } else { "$Value" }
            $status = if (& $Test $Value) { 'PASS' } else { 'FAIL' }
        }
        $rows.Add([pscustomobject]@{ Status = $status; Metric = $Metric; Value = $shown; Target = $Target; Source = $Source; Detail = $Detail })
    }

    # ---- spec 9 rows ----
    Add-Row 'Smoke (spec 10)' $(if ($smoke) { "{0} files, {1} failed" -f $smoke.total, $smoke.failed } else { $null }) 'all alive + responding' { $smoke.failed -eq 0 } 'smoke'
    $coldDetail = if ($ram) { "Cold = daemon only, 12 s after start (after the idle trim); 5 s value $($ram.coldAt5sMB) MB" } else { '' }
    Add-Row 'Cold (daemon only), MB' (Num $ram.coldMB) '<= 5' { param($v) $v -le 5 } 'measure-ram' -Detail $coldDetail
    $warmDetail = if ($ram) { "at 12 s $($ram.warmIdleAt12sMB), at 35 s $($ram.warmIdleAt35sMB); at 35 s daemon $($ram.warmDaemonMB) + Viewer $($ram.warmViewerMB) MB" } else { '' }
    Add-Row 'Warm-idle, MB (max of 12 s / 35 s)' (Num $ram.warmIdleMB) '<= 25' { param($v) $v -le 25 } 'measure-ram' -Detail $warmDetail
    Add-Row 'Warm-idle: prewarmed Viewer present' $(if ($ram) { "$($ram.warmViewerPresent)" } else { $null }) 'True' { param($v) $v -eq 'True' } 'measure-ram'
    Add-Row 'Warm-idle after use, MB (3 s after close)' (Num $ram.warmAfterUseMB) '<= 25' { param($v) $v -le 25 } 'measure-ram'
    Add-Row 'Space -> window (warm) p50, ms' (Num $e2e.warmWindowMs.p50) '<= 50' { param($v) $v -le 50 } 'e2e'
    Add-Row 'Space -> image rendered (warm) p50, ms' (Num $e2e.warmContentMs.image.p50) '<= 150' { param($v) $v -le 150 } 'e2e'
    Add-Row 'Space -> text rendered (warm) p50, ms' (Num $e2e.warmContentMs.text.p50) '<= 150' { param($v) $v -le 150 } 'e2e'
    Add-Row 'Space -> window (cold) p50, ms' (Num $cold.coldWindowMs.p50) '<= 400' { param($v) $v -le 400 } 'e2e -Cold'
    Add-Row '24 MP photo, MB (daemon + Viewer)' (Num $ram.photoMB) '<= 120' { param($v) $v -le 120 } 'measure-ram'
    $bs = Split-Growth $bench
    $bsOwnDetail = if ($bs) { "raw {0} MB, of it {1} MB in the {2} of {3} intervals with a driver step (driver bookkeeping in the process heap); non-heap private commit {4} MB; handles {5:+#;-#;0}; Private Bytes {6} MB" -f $bs.ownRaw, $bs.ownInSteps, $bs.driverSteps, $bs.intervals, $bs.nonHeap, $bs.handles, $bs.privateBytes } else { '' }
    Add-Row ('{0} opens: own memory growth (heap busy, 100 -> end, outside driver steps), MB' -f $BenchIterations) (Num $bs.own) '<= 2' { param($v) $v -le 2 } 'bench' -Detail $bsOwnDetail
    $bsDrvDetail = if ($bs) { "{0} step(s) of +{1} MB in total; outside the steps {2} MB" -f $bs.driverSteps, $bs.driverStepMB, $bs.driverRest } else { '' }
    Add-Row ('{0} opens: graphics driver pool growth (D3DKMT, 100 -> end), MB' -f $BenchIterations) (Num $bs.driver) 'reported (bounded)' { $true } 'bench' -Info -Detail $bsDrvDetail
    $failShown = if ($bench) { "{0} / {1}" -f $bench.failures, $(if ((Get-ExitText 'bench') -eq 'stored') { 0 } else { Get-ExitText 'bench' }) } else { $null }
    Add-Row ('{0} opens: failures / exit code' -f $BenchIterations) $failShown '0 / 0' { $bench.failures -eq 0 -and $failShown -eq '0 / 0' } 'bench'
    Add-Row ('{0} spaces: missed / crashes' -f $E2EIterations) $(if ($e2e) { "{0} / {1}" -f $e2e.missed, $e2e.crashes } else { $null }) '0 / 0' { $e2e.missed -eq 0 -and $e2e.crashes -eq 0 } 'e2e'
    Add-Row '24 h with sleep/lock: Space always works' $null 'always' { $true } 'manual' -Manual

    # ---- carried-over rows ----
    Add-Row 'Hook callback Space path p99, us' (Num $e2e.hookCallbackUs.space.p99) '<= 250' { param($v) $v -le 250 } 'e2e'
    Add-Row 'Hook callback other keys p99, us' (Num $e2e.hookCallbackUs.otherKeys.p99) '<= 50' { param($v) $v -le 50 } 'e2e'
    Add-Row 'Hook callback right after daemon trim p99, us' (Num $ram.hookAfterTrimUs.p99) '<= 50 (weak: F15 only, n small)' { param($v) $v -le 50 } 'measure-ram'
    Add-Row 'Warm-idle after daemon trim, MB (12 s after close)' (Num $ram.warmAfterTrimMB) '<= 25' { param($v) $v -le 25 } 'measure-ram'
    Add-Row 'After video preview + trim, MB (daemon + Viewer)' (Num $ram.videoAfterTrimMB) '<= 25' { param($v) $v -le 25 } 'measure-ram'
    $vs = Split-Growth $video
    $vsOwnDetail = if ($vs) { "raw {0} MB, of it {1} MB in the {2} of {3} intervals with a driver step; non-heap private commit {4} MB; handles {5:+#;-#;0}; Private Bytes {6} MB" -f $vs.ownRaw, $vs.ownInSteps, $vs.driverSteps, $vs.intervals, $vs.nonHeap, $vs.handles, $vs.privateBytes } else { '' }
    Add-Row ('Repeated video opens ({0}): own memory growth (heap busy, outside driver steps), MB' -f $VideoIterations) (Num $vs.own) '<= 2' { param($v) $v -le 2 } 'bench (video)' -Detail $vsOwnDetail
    $vsDrvDetail = if ($vs) { "{0} step(s) of +{1} MB in total; outside the steps {2} MB" -f $vs.driverSteps, $vs.driverStepMB, $vs.driverRest } else { '' }
    Add-Row ('Repeated video opens ({0}): graphics driver pool growth (D3DKMT), MB' -f $VideoIterations) (Num $vs.driver) 'reported (bounded)' { $true } 'bench (video)' -Info -Detail $vsDrvDetail
    Add-Row 'Repeated video opens: failures' (Num $video.failures) '0' { param($v) $v -eq 0 } 'bench (video)'
    Add-Row '24 MP + 24 MP neighbours, MB (Viewer + cold daemon)' (Num $ram.neighboursTotalMB) '<= 120' { param($v) $v -le 120 } 'measure-ram'
    Add-Row 'anim_start while neighbours decode, max ms' (Num $ram.animStartMs.max) '<= 50 (derived)' { param($v) $v -le 50 } 'measure-ram'
    Add-Row 'anim_in while neighbours decode, max ms (180 ms animation)' (Num $ram.animInMs.max) '<= 250 (derived)' { param($v) $v -le 250 } 'measure-ram'
    , $rows
}

# Acceptance numbers are only comparable on AC power: the power state goes into the JSON and the console output.
function Get-PowerState {
    Add-Type -AssemblyName System.Windows.Forms
    $line = [string][Windows.Forms.SystemInformation]::PowerStatus.PowerLineStatus
    $plan = ((powercfg /getactivescheme) -replace '^.*\((.*)\).*$', '$1')
    [ordered]@{ powerLine = $line; powerPlan = $plan }
}

function Save-Result {
    $rows = (Build-Rows)
    $failed = @($rows | Where-Object Status -eq 'FAIL')
    $exitTexts = [ordered]@{}; foreach ($n in $allSteps) { $exitTexts[$n] = Get-ExitText $n }
    [ordered]@{
        date = (Get-Date -Format 'o'); power = (Get-PowerState); configuration = $Configuration; failed = $failed.Count; stepsRun = $selected
        steps = $state; exitCodes = $exitTexts; rows = $rows
    } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $repo 'out\perf-acceptance.json') -Encoding utf8
    $rows
}

foreach ($name in $allSteps) {
    if ($selected -notcontains $name) { Write-Host "== ${name}: not selected (stored JSON, if any, is used)"; continue }
    $state[$name] = @{ ran = $true; exit = $null; note = '' }
    Remove-Item $json[$name] -ErrorAction SilentlyContinue
    if (-not (Wait-FreeRam)) {
        $state[$name].note = "skipped: free RAM below $MinFreeRamMB MB for 10 min"
        Write-Host "== ${name}: $($state[$name].note)"
        $null = Save-Result
        continue
    }
    if ($name -eq 'video') {
        & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'make-perf-fixtures.ps1') | Out-Null
        $dir = Join-Path $repo 'out\bench-video'
        New-Item -ItemType Directory -Force $dir | Out-Null
        if (-not (Test-Path (Join-Path $dir 'video-1080p.mp4'))) { Copy-Item (Join-Path $repo 'out\fixtures\video-1080p.mp4') $dir }
    }
    $step = $steps[$name]
    Write-Host "== ${name}: pwsh $($step.script) $($step.args -join ' ')"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        & pwsh -NoProfile -File (Join-Path $PSScriptRoot $step.script) @($step.args) *>&1 | Tee-Object -FilePath (Join-Path $perf "$name.log") | ForEach-Object { Write-Host "   $_" }
        $state[$name].exit = $LASTEXITCODE
    }
    catch {
        $state[$name].note = "threw: $_"
    }
    Write-Host ("== {0}: exit {1} in {2:n0} s {3}" -f $name, $state[$name].exit, $sw.Elapsed.TotalSeconds, $state[$name].note)
    $null = Save-Result
}

$rows = Save-Result
Write-Host ''
$power = Get-PowerState
Write-Host ('Power: {0} ({1}){2}' -f $power.powerLine, $power.powerPlan, $(if ($power.powerLine -ne 'Online') { '  WARNING: not on AC, CPU-bound rows are not comparable' } else { '' }))
Write-Host ('Performance acceptance ({0}), {1}; steps run: {2}' -f $Configuration, (Get-Date -Format 'yyyy-MM-dd HH:mm'), $(if ($selected.Count) { $selected -join ',' } else { 'none (stored data)' }))
$rows | Format-Table Status, Metric, Value, Target, Source -AutoSize | Out-String -Width 220 | Write-Host
$rows | Where-Object Detail | ForEach-Object { Write-Host ("note: {0}: {1}" -f $_.Metric, $_.Detail) }
$failed = @($rows | Where-Object Status -eq 'FAIL')
Write-Host "JSON: $(Join-Path $repo 'out\perf-acceptance.json')"
if ($failed.Count -gt 0) { Write-Host ("RESULT: FAIL ({0} row(s))" -f $failed.Count); exit 1 }
Write-Host 'RESULT: PASS'
exit 0
