#requires -Version 7
<#
.SYNOPSIS
  Black-box check of the daemon keyboard hook self-healing (Task 27): install, TaskbarCreated rehook, stalled-thread watchdog.
.DESCRIPTION
  Runs FastLook.exe with an isolated FASTLOOK_DATA_DIR, prints PASS/FAIL per check and exits non-zero on any FAIL.
  Injects no keys, so it does not depend on PowerToys Peek's state. Everything it launches is killed in `finally`.
  Refuses to run while another FastLook.exe is alive (the single-instance mutex is per session, not per data dir).
  The stall phase freezes the hook thread for ~7 s with the hook installed: system-wide typing may lag briefly meanwhile.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'lib\Win32.psm1') -Force

$exe = Join-Path $repo "out\bin\$Configuration\FastLook.exe"
if (-not (Test-Path $exe)) { throw "Not built: $exe (run scripts/build.ps1 first)" }
if (Get-Process -Name FastLook -ErrorAction SilentlyContinue) {
    throw 'Another FastLook.exe is running; stop it first (the daemon mutex is per session).'
}

Add-Type -Namespace FastLookTest -Name Msg -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessageW(string name);
[DllImport("user32.dll")] public static extern bool PostMessageW(System.IntPtr hwnd, uint msg, System.IntPtr w, System.IntPtr l);
'@

$script:failed = $false
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    if ($Ok) { Write-Host "PASS $Name" }
    else { Write-Host "FAIL $Name $Detail"; $script:failed = $true }
}

function Wait-LogMatch([string]$Pattern, [int]$Count = 1, [int]$TimeoutMs = 5000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        $text = ''
        if (Test-Path $script:logFile) {
            try { $text = [IO.File]::ReadAllText($script:logFile) } catch { }
        }
        if ([regex]::Matches($text, $Pattern).Count -ge $Count) { return $true }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Stop-Daemon($Proc) {
    if ($Proc -and -not $Proc.HasExited) {
        $sd = Start-Process -FilePath $exe -ArgumentList '--shutdown' -PassThru -Wait
        if (-not $Proc.WaitForExit(5000)) { $Proc.Kill() }
    }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("fastlook-hook-{0}" -f [guid]::NewGuid().ToString('N'))
$script:logFile = $null
$savedData = $env:FASTLOOK_DATA_DIR
$savedPt = $env:FASTLOOK_POWERTOYS_DIR
$savedStall = $env:FASTLOOK_TEST_STALL_HOOK_MS
$daemon = $null
try {
    # Empty PowerToys dir: no Peek conflict notice, no side effects on the user's configuration.
    $pt = Join-Path $tmp 'powertoys'
    New-Item -ItemType Directory -Force $pt | Out-Null
    $env:FASTLOOK_POWERTOYS_DIR = $pt

    # Phase 1: install, TaskbarCreated rehook.
    $data = Join-Path $tmp 'data1'
    New-Item -ItemType Directory -Force $data | Out-Null
    $script:logFile = Join-Path $data 'logs\daemon.log'
    $env:FASTLOOK_DATA_DIR = $data
    Remove-Item Env:FASTLOOK_TEST_STALL_HOOK_MS -ErrorAction SilentlyContinue
    $daemon = Start-Process -FilePath $exe -PassThru
    Check 'log: hook installed' (Wait-LogMatch 'hook installed')
    Check 'log: tray added' (Wait-LogMatch 'tray added')

    $msg = [FastLookTest.Msg]::RegisterWindowMessageW('TaskbarCreated')
    [void][FastLookTest.Msg]::PostMessageW([IntPtr]0xFFFF, $msg, [IntPtr]::Zero, [IntPtr]::Zero)
    Check 'TaskbarCreated: rehook reason=taskbar' (Wait-LogMatch 'rehook reason=taskbar')
    Check 'TaskbarCreated: tray added again' (Wait-LogMatch 'tray added' 2)
    Check 'TaskbarCreated: hook installed again' (Wait-LogMatch 'hook installed' 2)
    Check 'log: hook_cb_max_us reported' (Wait-LogMatch 'hook installed hook_cb_max_us=\d+')
    Check 'alive after rehook' (-not $daemon.HasExited)
    Stop-Daemon $daemon
    Check 'phase 1 daemon exited' $daemon.HasExited
    $daemon = $null

    # Phase 2: the hook thread sleeps 7 s starting 3 s after launch; the watchdog must replace it.
    $data = Join-Path $tmp 'data2'
    New-Item -ItemType Directory -Force $data | Out-Null
    $script:logFile = Join-Path $data 'logs\daemon.log'
    $env:FASTLOOK_DATA_DIR = $data
    $env:FASTLOOK_TEST_STALL_HOOK_MS = '7000'
    $daemon = Start-Process -FilePath $exe -PassThru
    Check 'stall: first hook installed' (Wait-LogMatch 'hook installed')
    Check 'stall: watchdog restart within 10 s of the stall' (Wait-LogMatch 'hook watchdog restart' 1 13000)
    Check 'stall: hook installed again after restart' (Wait-LogMatch 'hook installed' 2 3000)
    Start-Sleep -Seconds 6 # let the abandoned thread wake up and exit
    Check 'stall: daemon still alive' (-not $daemon.HasExited)
    $ws = Get-PrivateWorkingSetMB -ProcessId $daemon.Id
    Check 'private WS <= 5 MB' ($ws -le 5) ("({0:N1} MB)" -f $ws)
    Stop-Daemon $daemon
    Check 'phase 2 daemon exited' $daemon.HasExited
    $daemon = $null
}
catch {
    Write-Host "FAIL unexpected error: $_"
    $script:failed = $true
}
finally {
    if ($daemon -and -not $daemon.HasExited) { $daemon.Kill() }
    Get-Process -Name FastLook -ErrorAction SilentlyContinue | Stop-Process -Force
    $env:FASTLOOK_DATA_DIR = $savedData
    $env:FASTLOOK_POWERTOYS_DIR = $savedPt
    $env:FASTLOOK_TEST_STALL_HOOK_MS = $savedStall
    if ($script:failed -and $script:logFile -and (Test-Path $script:logFile)) { Get-Content $script:logFile | Write-Host }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($script:failed) { Write-Host 'FAIL'; exit 1 }
Write-Host 'PASS'
exit 0
