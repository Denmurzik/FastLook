#requires -Version 7
<#
.SYNOPSIS
  Black-box check of the daemon shell (Task 26): single instance, tray, Peek notice, CLI forwarding, TaskbarCreated, shutdown.
.DESCRIPTION
  Runs FastLook.exe with an isolated FASTLOOK_DATA_DIR and a fake PowerToys config (Peek enabled on Space), prints PASS/FAIL
  per check and exits non-zero on any FAIL. Everything it launches is killed in `finally`.
  Refuses to run while another FastLook.exe is alive (the single-instance mutex is per session, not per data dir).
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
        $text = if (Test-Path $script:logFile) { [IO.File]::ReadAllText($script:logFile) } else { '' }
        if ([regex]::Matches($text, $Pattern).Count -ge $Count) { return $true }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("fastlook-daemon-{0}" -f [guid]::NewGuid().ToString('N'))
$data = Join-Path $tmp 'data'
$pt = Join-Path $tmp 'powertoys'
New-Item -ItemType Directory -Force $data, (Join-Path $pt 'Peek') | Out-Null
Copy-Item (Join-Path $repo 'tests\fixtures\peek\global-enabled.json') (Join-Path $pt 'settings.json')
Copy-Item (Join-Path $repo 'tests\fixtures\peek\peek-space.json') (Join-Path $pt 'Peek\settings.json')
$script:logFile = Join-Path $data 'logs\daemon.log'

$savedData = $env:FASTLOOK_DATA_DIR
$savedPt = $env:FASTLOOK_POWERTOYS_DIR
$env:FASTLOOK_DATA_DIR = $data
$env:FASTLOOK_POWERTOYS_DIR = $pt
$daemon = $null
try {
    $daemon = Start-Process -FilePath $exe -PassThru
    Start-Sleep -Seconds 3
    Check 'alive after 3s' (-not $daemon.HasExited)
    Check 'log: tray added' (Wait-LogMatch 'tray added')
    Check 'log: peek conflict' (Wait-LogMatch 'peek conflict')

    $ws = Get-PrivateWorkingSetMB -ProcessId $daemon.Id
    Check 'private WS <= 5 MB' ($ws -le 5) ("({0:N1} MB)" -f $ws)

    $second = Start-Process -FilePath $exe -PassThru -Wait
    Check 'second instance exits 0' ($second.ExitCode -eq 0) "(exit $($second.ExitCode))"
    Check 'log: openSettings' (Wait-LogMatch 'openSettings')

    $show = Start-Process -FilePath $exe -ArgumentList '--show', 'C:\x.txt' -PassThru -Wait
    Check '--show exits 0' ($show.ExitCode -eq 0) "(exit $($show.ExitCode))"
    Check 'log: showPath' (Wait-LogMatch 'showPath')

    $toggle = Start-Process -FilePath $exe -ArgumentList '--toggle', 'C:\x.txt' -PassThru -Wait
    Check '--toggle exits 0' ($toggle.ExitCode -eq 0) "(exit $($toggle.ExitCode))"
    Check 'log: togglePath' (Wait-LogMatch 'togglePath')

    $msg = [FastLookTest.Msg]::RegisterWindowMessageW('TaskbarCreated')
    [void][FastLookTest.Msg]::PostMessageW([IntPtr]0xFFFF, $msg, [IntPtr]::Zero, [IntPtr]::Zero)
    Check 'TaskbarCreated: rehook reason=taskbar' (Wait-LogMatch 'rehook reason=taskbar')
    Check 'TaskbarCreated: tray re-added' (Wait-LogMatch 'tray added' 2)

    $sd = Start-Process -FilePath $exe -ArgumentList '--shutdown' -PassThru -Wait
    Check '--shutdown exits 0' ($sd.ExitCode -eq 0) "(exit $($sd.ExitCode))"
    Check 'daemon gone within 5s' ($daemon.WaitForExit(5000))
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
    if ($script:failed -and (Test-Path $script:logFile)) { Get-Content $script:logFile | Write-Host }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($script:failed) { Write-Host 'FAIL'; exit 1 }
Write-Host 'PASS'
exit 0
