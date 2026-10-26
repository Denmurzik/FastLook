#requires -Version 7
<#
.SYNOPSIS
  Performance acceptance (spec 9): runs every measurement and prints a PASS/FAIL table (spec rows + carried-over rows).
.DESCRIPTION
  Runs, in order: smoke.ps1, measure-ram.ps1, bench.ps1 (mixed fixtures, -BenchIterations), bench.ps1 over a video-only folder
  (Private Bytes over repeated video opens), e2e-hook.ps1 -Iterations N, e2e-hook.ps1 -Cold. Each step's console output goes to
  out/perf/<step>.log, its JSON to out/perf/<step>.json; the table and all values go to out/perf-acceptance.json and out/perf/.
  Exit code 1 when any row is FAIL (or could not be measured); MANUAL rows (24 h sleep/lock) never fail the run.

  WARNING: measure-ram.ps1 and e2e-hook.ps1 take the desktop and inject keys (e2e-hook for ~10-15 min at -E2EIterations 1000).
  Do not touch keyboard or mouse while they run. Run through the test lock; the steps hold %TEMP%\gui-e2e.lock themselves.
  -Skip omits steps (smoke, ram, bench, video, e2e, cold); the table is then built from the JSON stored by an earlier run in
  out/perf/ (a row without stored data shows "not measured" and counts as FAIL).
#>
[CmdletBinding()]
param(
    [int]$BenchIterations = 1000,
    [int]$E2EIterations = 1000,
    [int]$VideoIterations = 200,
    [ValidateSet('smoke', 'ram', 'bench', 'video', 'e2e', 'cold')]
    [string[]]$Skip = @(),
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$perf = Join-Path $repo 'out\perf'
New-Item -ItemType Directory -Force $perf | Out-Null

function Run-Step([string]$Name, [string]$Script, [string[]]$StepArgs) {
    if ($Skip -contains $Name) { Write-Host "== ${Name}: skipped"; return $null }
    Write-Host "== ${Name}: pwsh $Script $($StepArgs -join ' ')"
    $log = Join-Path $perf "$Name.log"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot $Script) @StepArgs *>&1 | Tee-Object -FilePath $log | ForEach-Object { Write-Host "   $_" }
    $code = $LASTEXITCODE
    Write-Host ("== {0}: exit {1} in {2:n0} s" -f $Name, $code, $sw.Elapsed.TotalSeconds)
    $code
}
function Read-Json([string]$Path) {
    if (-not (Test-Path $Path)) { return $null }
    try { Get-Content $Path -Raw | ConvertFrom-Json } catch { $null }
}

$smokeJson = Join-Path $perf 'smoke.json'
$ramJson = Join-Path $perf 'measure-ram.json'
$benchJson = Join-Path $perf 'bench.json'
$videoJson = Join-Path $perf 'bench-video.json'
$e2eJson = Join-Path $perf 'e2e-warm.json'
$coldJson = Join-Path $perf 'e2e-cold.json'
$cfg = @('-Configuration', $Configuration)

$exit = @{}
$exit.smoke = Run-Step 'smoke' 'smoke.ps1' ($cfg + @('-OutJson', $smokeJson))
$exit.ram = Run-Step 'ram' 'measure-ram.ps1' ($cfg + @('-OutJson', $ramJson))
$exit.bench = Run-Step 'bench' 'bench.ps1' ($cfg + @('-Iterations', $BenchIterations, '-OutJson', $benchJson))

if ($Skip -notcontains 'video') {
    $videoDir = Join-Path $repo 'out\bench-video'
    New-Item -ItemType Directory -Force $videoDir | Out-Null
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'make-perf-fixtures.ps1') | Out-Null
    $v = Join-Path $videoDir 'video-1080p.mp4'
    if (-not (Test-Path $v)) { Copy-Item (Join-Path $repo 'out\fixtures\video-1080p.mp4') $v }
}
$exit.video = Run-Step 'video' 'bench.ps1' ($cfg + @('-Iterations', $VideoIterations, '-Dir', (Join-Path $repo 'out\bench-video'), '-OutJson', $videoJson))
$exit.e2e = Run-Step 'e2e' 'e2e-hook.ps1' ($cfg + @('-Iterations', $E2EIterations, '-OutJson', $e2eJson))
$exit.cold = Run-Step 'cold' 'e2e-hook.ps1' ($cfg + @('-Cold', '-OutJson', $coldJson))
# A skipped step has no exit code of its own: bench.ps1 writes its JSON only after the Viewer ended, so stored JSON stands for exit 0.
$benchExit = if ($null -ne $exit.bench) { $exit.bench } else { 0 }

$smoke = Read-Json $smokeJson
$ram = Read-Json $ramJson
$bench = Read-Json $benchJson
$video = Read-Json $videoJson
$e2e = Read-Json $e2eJson
$cold = Read-Json $coldJson

$rows = [System.Collections.Generic.List[object]]::new()
# $Value: number or text or $null (not measured); $Test: scriptblock receiving the value; $Manual: never fails.
function Add-Row([string]$Metric, $Value, [string]$Target, [scriptblock]$Test, [string]$Source, [switch]$Manual) {
    $status = 'FAIL'
    $shown = 'not measured'
    if ($Manual) { $status = 'MANUAL'; $shown = 'release checklist' }
    elseif ($null -ne $Value -and "$Value" -ne '') {
        $shown = if ($Value -is [double] -or $Value -is [float]) { '{0:n1}' -f $Value } else { "$Value" }
        $status = if (& $Test $Value) { 'PASS' } else { 'FAIL' }
    }
    $rows.Add([pscustomobject]@{ Status = $status; Metric = $Metric; Value = $shown; Target = $Target; Source = $Source })
}
function Num($x) { if ($null -eq $x -or "$x" -eq '') { $null } else { [double]$x } }

# ---- spec 9 rows ----
Add-Row 'Smoke (spec 10)' $(if ($smoke) { "{0} files, {1} failed" -f $smoke.total, $smoke.failed } else { $null }) 'every fixture alive + responding' { param($v) $smoke.failed -eq 0 } 'smoke'
Add-Row 'Cold (daemon only), MB' (Num $ram.coldMB) '<= 5' { param($v) $v -le 5 } 'measure-ram'
Add-Row 'Warm-idle, MB (max of 6 s / 35 s)' (Num $ram.warmIdleMB) '<= 25' { param($v) $v -le 25 } 'measure-ram'
Add-Row 'Warm-idle after use, MB (3 s after close)' (Num $ram.warmAfterUseMB) '<= 25' { param($v) $v -le 25 } 'measure-ram'
Add-Row 'Space -> window (warm) p50, ms' (Num $e2e.warmWindowMs.p50) '<= 50' { param($v) $v -le 50 } 'e2e'
Add-Row 'Space -> image rendered (warm) p50, ms' (Num $e2e.warmContentMs.image.p50) '<= 150' { param($v) $v -le 150 } 'e2e'
Add-Row 'Space -> text rendered (warm) p50, ms' (Num $e2e.warmContentMs.text.p50) '<= 150' { param($v) $v -le 150 } 'e2e'
Add-Row 'Space -> window (cold) p50, ms' (Num $cold.coldWindowMs.p50) '<= 400' { param($v) $v -le 400 } 'e2e -Cold'
Add-Row '24 MP photo, MB (daemon + Viewer)' (Num $ram.photoMB) '<= 120' { param($v) $v -le 120 } 'measure-ram'
$growth = if ($bench -and $null -ne $bench.privateBytesEndMB) { [math]::Round([double]$bench.privateBytesEndMB - [double]$bench.privateBytesAt100MB, 2) } else { $null }
Add-Row ('{0} opens: Private Bytes growth (100 -> end), MB' -f $BenchIterations) (Num $growth) '<= 2' { param($v) $v -le 2 } 'bench'
Add-Row ('{0} opens: failures / exit code' -f $BenchIterations) $(if ($bench) { "{0} / {1}" -f $bench.failures, $benchExit } else { $null }) '0 / 0' { param($v) $bench.failures -eq 0 -and $benchExit -eq 0 } 'bench'
Add-Row ('{0} spaces: missed / crashes' -f $E2EIterations) $(if ($e2e) { "{0} / {1}" -f $e2e.missed, $e2e.crashes } else { $null }) '0 / 0' { param($v) $e2e.missed -eq 0 -and $e2e.crashes -eq 0 } 'e2e'
Add-Row '24 h with sleep/lock: Space always works' $null 'always' { $true } 'manual' -Manual

# ---- carried-over rows ----
Add-Row 'Hook callback Space path p99, us' (Num $e2e.hookCallbackUs.space.p99) '<= 250' { param($v) $v -le 250 } 'e2e'
Add-Row 'Hook callback other keys p99, us' (Num $e2e.hookCallbackUs.otherKeys.p99) '<= 50' { param($v) $v -le 50 } 'e2e'
Add-Row 'Hook callback right after daemon trim p99, us (F15)' (Num $ram.hookAfterTrimUs.p99) '<= 50' { param($v) $v -le 50 } 'measure-ram'
Add-Row 'Warm-idle after daemon trim, MB (12 s after close)' (Num $ram.warmAfterTrimMB) '<= 25' { param($v) $v -le 25 } 'measure-ram'
Add-Row 'After video preview + trim, MB (daemon + Viewer)' (Num $ram.videoAfterTrimMB) '<= 25' { param($v) $v -le 25 } 'measure-ram'
$vGrowth = if ($video -and $null -ne $video.privateBytesEndMB) { [math]::Round([double]$video.privateBytesEndMB - [double]$video.privateBytesAt100MB, 2) } else { $null }
Add-Row ('Repeated video opens ({0}): Private Bytes growth (100 -> end), MB' -f $VideoIterations) (Num $vGrowth) '<= 2' { param($v) $v -le 2 } 'bench (video)'
Add-Row 'Repeated video opens: failures' (Num $video.failures) '0' { param($v) $v -eq 0 } 'bench (video)'
Add-Row '24 MP + 24 MP neighbours, MB (Viewer + cold daemon)' (Num $ram.neighboursTotalMB) '<= 120' { param($v) $v -le 120 } 'measure-ram'
Add-Row 'anim_start while neighbours decode, max ms' (Num $ram.animStartMs.max) '<= 50 (derived)' { param($v) $v -le 50 } 'measure-ram'
Add-Row 'anim_in while neighbours decode, max ms (180 ms animation)' (Num $ram.animInMs.max) '<= 250 (derived)' { param($v) $v -le 250 } 'measure-ram'

Write-Host ''
Write-Host ('Performance acceptance ({0}), {1}' -f $Configuration, (Get-Date -Format 'yyyy-MM-dd HH:mm'))
$rows | Format-Table Status, Metric, Value, Target, Source -AutoSize | Out-String -Width 220 | Write-Host

$failed = @($rows | Where-Object Status -eq 'FAIL')
$result = [ordered]@{
    date = (Get-Date -Format 'o'); configuration = $Configuration; failed = $failed.Count
    rows = $rows; exitCodes = $exit
    bench = $bench; benchVideo = $video; ram = $ram; e2e = $e2e; coldE2e = $cold; smoke = $smoke
}
$outFull = Join-Path $repo 'out\perf-acceptance.json'
$result | ConvertTo-Json -Depth 12 | Set-Content $outFull -Encoding utf8
Write-Host "JSON: $outFull"
if ($failed.Count -gt 0) { Write-Host ("RESULT: FAIL ({0} row(s))" -f $failed.Count); exit 1 }
Write-Host 'RESULT: PASS'
exit 0
