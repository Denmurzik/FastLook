#requires -Version 7
<#
.SYNOPSIS
  Black-box checks of the daemon preview controller (Task 29): --warm prewarm, --show, Viewer crash / hang / stale recovery,
  the idle-exit race and shutdown. Prints PASS/FAIL lines and a SUMMARY.
.DESCRIPTION
  Needs built Release FastLook.exe + FastLook.Viewer.exe and a free desktop (it shows windows and reads the foreground window).
  Uses a temp FASTLOOK_DATA_DIR; `finally` kills every FastLook* process. Refuses to run while FastLook* processes exist
  (the daemon mutex is per session). Exit code 1 when any check failed.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [int]$RaceIterations = 30
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
Import-Module (Join-Path $PSScriptRoot 'lib\Win32.psm1') -Force

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class T29
{
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")] public static extern uint SuspendThread(IntPtr h);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    public static int SuspendAll(System.Diagnostics.Process p)
    {
        int n = 0;
        foreach (System.Diagnostics.ProcessThread t in p.Threads)
        {
            IntPtr h = OpenThread(0x0002, false, (uint)t.Id); // THREAD_SUSPEND_RESUME
            if (h == IntPtr.Zero) continue;
            if (SuspendThread(h) != 0xFFFFFFFF) n++;
            CloseHandle(h);
        }
        return n;
    }
}
'@

$exe = Join-Path $repo "out\bin\$Configuration\FastLook.exe"
$viewerExe = Join-Path $repo "out\bin\$Configuration\FastLook.Viewer.exe"
foreach ($f in $exe, $viewerExe) { if (-not (Test-Path $f)) { throw "Not built: $f (run scripts/build.ps1 first)" } }
if (Get-Process -Name 'FastLook*' -ErrorAction SilentlyContinue) {
    throw 'FastLook* processes are running; stop them first (the daemon mutex is per session).'
}

$fixtures = Join-Path $repo 'tests\fixtures\shell'
$a = (Resolve-Path (Join-Path $fixtures 'a.txt')).Path
$b = (Resolve-Path (Join-Path $fixtures 'b.txt')).Path
$c = (Resolve-Path (Join-Path $fixtures 'c.txt')).Path
$shots = Join-Path $repo 'out\shots'

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("fastlook-t29-{0}" -f [guid]::NewGuid().ToString('N'))
$data = Join-Path $tmp 'data'
$emptyPt = Join-Path $tmp 'powertoys'
New-Item -ItemType Directory -Force $data, $emptyPt | Out-Null
$logFile = Join-Path $data 'logs\daemon.log'

$script:failed = $false
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    if ($Ok) { Write-Host "PASS $Name $Detail" } else { Write-Host "FAIL $Name $Detail"; $script:failed = $true }
}
function LogText { if (Test-Path $logFile) { [IO.File]::ReadAllText($logFile) } else { '' } }
function LogCount([string]$Pattern) { [regex]::Matches((LogText), $Pattern).Count }
function Wait-Log([string]$Pattern, [int]$TimeoutMs = 5000, [int]$Count = 1) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        if ((LogCount $Pattern) -ge $Count) { return $true }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    $false
}
function ViewerProcs { @(Get-Process -Name 'FastLook.Viewer' -ErrorAction SilentlyContinue) }
# A Viewer process (not in $Exclude) with a visible window; $null on timeout.
function Wait-NewViewerWindow([int[]]$Exclude, [int]$TimeoutMs) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        foreach ($p in ViewerProcs) {
            if ($Exclude -contains $p.Id) { continue }
            try { $h = Wait-TopWindow -ProcessId $p.Id -TimeoutSec 0; return [pscustomobject]@{ Proc = $p; Hwnd = $h } } catch { }
        }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    $null
}
function Invoke-Daemon([string[]]$CliArgs) { $p = Start-Process -FilePath $exe -ArgumentList $CliArgs -PassThru -Wait -WindowStyle Hidden; $p.ExitCode }
function Start-Daemon([string[]]$DaemonArgs = @()) {
    if ($DaemonArgs.Count) { Start-Process -FilePath $exe -ArgumentList $DaemonArgs -PassThru } else { Start-Process -FilePath $exe -PassThru }
}
function Stop-All { Get-Process -Name 'FastLook*' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }

$saved = @{}
foreach ($n in 'FASTLOOK_DATA_DIR', 'FASTLOOK_POWERTOYS_DIR', 'FASTLOOK_IDLE_SECONDS') { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
$env:FASTLOOK_DATA_DIR = $data
$env:FASTLOOK_POWERTOYS_DIR = $emptyPt
$env:FASTLOOK_IDLE_SECONDS = $null
$daemon = $null
try {
    # ---------- --warm ----------
    $fgBefore = [T29]::GetForegroundWindow()
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $daemon = Start-Daemon @('--warm')
    $viewer = $null
    while ($sw.ElapsedMilliseconds -lt 2000 -and -not $viewer) { $viewer = ViewerProcs | Select-Object -First 1; Start-Sleep -Milliseconds 50 }
    Check 'warm: Viewer process within 2 s' ($null -ne $viewer) ("({0} ms)" -f $sw.ElapsedMilliseconds)
    while ($sw.ElapsedMilliseconds -lt 4000) { Start-Sleep -Milliseconds 100 }
    Check 'warm: foreground unchanged' ($fgBefore -eq [T29]::GetForegroundWindow())
    if ($viewer) {
        $viewer.Refresh()
        $total = (Get-PrivateWorkingSetMB -ProcessId $daemon.Id) + (Get-PrivateWorkingSetMB -ProcessId $viewer.Id)
        Check 'warm: private WS daemon + Viewer <= 25 MB at 4 s' ($total -le 25) ("({0:N1} MB)" -f $total)
    }

    # ---------- --show ----------
    $before = @((ViewerProcs).Id)
    Check '--show a.txt exits 0' ((Invoke-Daemon @('--show', $a)) -eq 0)
    $w = Wait-NewViewerWindow @() 1000
    Check 'show a: Viewer window visible within 1 s' ($null -ne $w)
    if ($w) { Start-Sleep -Milliseconds 400; $null = Save-WindowPng -Hwnd $w.Hwnd -Out (Join-Path $shots 'task29-a.png'); Write-Host 'shot: out\shots\task29-a.png' }
    $pidA = if ($w) { $w.Proc.Id } else { 0 }

    # ---------- Viewer killed ----------
    ViewerProcs | Stop-Process -Force
    Start-Sleep -Milliseconds 200
    $null = Invoke-Daemon @('--show', $b)
    $w = Wait-NewViewerWindow @($pidA) 2000
    Check 'killed Viewer: new Viewer shows b.txt within 2 s' ($null -ne $w) $(if ($w) { "(pid $($w.Proc.Id))" })
    if ($w) { Start-Sleep -Milliseconds 400; $null = Save-WindowPng -Hwnd $w.Hwnd -Out (Join-Path $shots 'task29-b.png'); Write-Host 'shot: out\shots\task29-b.png' }
    $pidB = if ($w) { $w.Proc.Id } else { 0 }

    # ---------- Viewer hung (all threads suspended) ----------
    $hung = ViewerProcs | Where-Object { $_.Id -eq $pidB } | Select-Object -First 1
    $n = if ($hung) { [T29]::SuspendAll($hung) } else { 0 }
    Check 'hang: threads suspended' ($n -gt 0) "($n)"
    $noAckBefore = LogCount 'viewer kill reason=no ack'
    $null = Invoke-Daemon @('--show', $c)
    $w = Wait-NewViewerWindow @($pidA, $pidB) 3000
    Check 'hang: new Viewer shows c.txt within 3 s' ($null -ne $w) $(if ($w) { "(pid $($w.Proc.Id))" })
    Check 'hang: log has no ack kill' ((LogCount 'viewer kill reason=no ack') -gt $noAckBefore)
    Check 'hang: suspended Viewer is gone' (-not (Get-Process -Id $pidB -ErrorAction SilentlyContinue))
    if ($w) { Start-Sleep -Milliseconds 400; $null = Save-WindowPng -Hwnd $w.Hwnd -Out (Join-Path $shots 'task29-c.png'); Write-Host 'shot: out\shots\task29-c.png' }
    $pidC = if ($w) { $w.Proc.Id } else { 0 }

    # ---------- stale heartbeat ----------
    $stale = ViewerProcs | Where-Object { $_.Id -eq $pidC } | Select-Object -First 1
    $n = if ($stale) { [T29]::SuspendAll($stale) } else { 0 }
    Check 'stale: threads suspended' ($n -gt 0) "($n)"
    Check 'stale: log "viewer stale" within 16 s' (Wait-Log 'viewer stale' 16000)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $null = Invoke-Daemon @('--show', $a)
    $w = Wait-NewViewerWindow @($pidA, $pidB, $pidC) 1500
    Check 'stale: new Viewer shows a.txt within 1.5 s' ($null -ne $w) ("({0} ms)" -f $sw.ElapsedMilliseconds)
    Check 'stale: log has stale kill' ((LogCount 'viewer kill reason=stale') -ge 1)
    Check 'stale: suspended Viewer is gone' (-not (Get-Process -Id $pidC -ErrorAction SilentlyContinue))

    # ---------- shutdown with a live Viewer ----------
    $viewers = ViewerProcs
    Check 'shutdown: Viewer alive before' ($viewers.Count -ge 1)
    Check '--shutdown exits 0' ((Invoke-Daemon @('--shutdown')) -eq 0)
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline -and ((ViewerProcs).Count -gt 0 -or -not $daemon.HasExited)) { Start-Sleep -Milliseconds 100 }
    Check 'shutdown: daemon and Viewer gone within 5 s' ($daemon.HasExited -and (ViewerProcs).Count -eq 0)
    Stop-All

    # ---------- idle-exit race ----------
    Remove-Item (Join-Path $data 'logs') -Recurse -Force -ErrorAction SilentlyContinue
    $env:FASTLOOK_IDLE_SECONDS = '1'
    $daemon = Start-Daemon
    Start-Sleep -Seconds 2
    for ($i = 0; $i -lt $RaceIterations; $i++) {
        $null = Invoke-Daemon @('--toggle', $a)
        Start-Sleep -Milliseconds 300
        $null = Invoke-Daemon @('--toggle', $a)
        Start-Sleep -Milliseconds (Get-Random -Minimum 900 -Maximum 1201)
    }
    Start-Sleep -Seconds 3
    $shown1 = LogCount 'toggle_acked shown=1'
    $shown0 = LogCount 'toggle_acked shown=0'
    Check "race: $RaceIterations x toggle_acked shown=1" ($shown1 -eq $RaceIterations) "(got $shown1)"
    Check "race: $RaceIterations x toggle_acked shown=0" ($shown0 -eq $RaceIterations) "(got $shown0)"
    Check 'race: no deliver failed' ((LogCount 'deliver failed') -eq 0)
    $env:FASTLOOK_IDLE_SECONDS = $null

    Check 'final --shutdown exits 0' ((Invoke-Daemon @('--shutdown')) -eq 0)
    Check 'final: daemon gone within 5 s' ($daemon.WaitForExit(5000))
}
catch {
    Write-Host "FAIL unexpected error: $_"
    $script:failed = $true
}
finally {
    Stop-All
    foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
    if ($script:failed -and (Test-Path $logFile)) { Write-Host '--- daemon.log ---'; Get-Content $logFile -Tail 80 | Write-Host }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($script:failed) { Write-Host 'SUMMARY: FAIL'; exit 1 }
Write-Host 'SUMMARY: PASS'
exit 0
