#requires -Version 7
<#
.SYNOPSIS
  UI verification: launches a FastLook exe, screenshots its window, samples memory, prints a RESULT line, always kills it.
.DESCRIPTION
  Launches `<Exe> --preview <File> --perf-log <tmp> <ExtraArgs>` (without --preview when -File is omitted) with
  FASTLOOK_DATA_DIR pointing at a fresh temp directory (unless -KeepEnv), waits for a top-level window, waits -WaitMs,
  saves a PNG, samples Private Working Set and prints:
    RESULT alive=<bool> window=<bool> responding=<bool> privateWS_MB=<n> png=<path>
  followed by the perf log, if any. Exit code 1 when the process died, or (Viewer only) never showed a window.
  -KeepEnv leaves FASTLOOK_DATA_DIR untouched (real settings) and keeps nothing else; the temp perf log is still removed.
.EXAMPLE
  pwsh scripts/run-preview.ps1 -File tests/fixtures/x.txt -Out out/shots/x.png
#>
[CmdletBinding()]
param(
    [string]$File,
    [string[]]$ExtraArgs = @(),
    [string]$Out,
    [int]$WaitMs = 1500,
    [ValidateSet('Viewer', 'Daemon')]
    [string]$Exe = 'Viewer',
    [switch]$KeepEnv,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [int]$WindowTimeoutSec = 10
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'lib\Win32.psm1') -Force

$exeName = if ($Exe -eq 'Viewer') { 'FastLook.Viewer.exe' } else { 'FastLook.exe' }
$exePath = Join-Path $repo "out\bin\$Configuration\$exeName"
if (-not (Test-Path $exePath)) { throw "Not built: $exePath (run scripts/build.ps1 first)" }
if (-not $Out) { $Out = Join-Path $repo 'out\shots\run-preview.png' }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("fastlook-run-{0}" -f [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null
$perfLog = Join-Path $tmp 'perf.log'

$argList = @()
if ($File) { $argList += @('--preview', (Resolve-Path $File).Path) }
$argList += @('--perf-log', $perfLog)
$argList += $ExtraArgs

$savedDataDir = $env:FASTLOOK_DATA_DIR
if (-not $KeepEnv) { $env:FASTLOOK_DATA_DIR = Join-Path $tmp 'data' }

$proc = $null
$alive = $false; $window = $false; $responding = $false; $ws = 'n/a'
try {
    $proc = Start-Process -FilePath $exePath -ArgumentList $argList -PassThru
    $hwnd = $null
    try { $hwnd = Wait-TopWindow -ProcessId $proc.Id -TimeoutSec $WindowTimeoutSec } catch { }
    $window = $null -ne $hwnd
    if ($window) {
        Start-Sleep -Milliseconds $WaitMs
        $method = Save-WindowPng -Hwnd $hwnd -Out $Out
        Write-Host "Screenshot: $Out ($method)"
    }
    $proc.Refresh()
    $alive = -not $proc.HasExited
    if ($alive) {
        $responding = $proc.Responding
        $ws = '{0:n1}' -f (Get-PrivateWorkingSetMB -ProcessId $proc.Id)
    }
}
finally {
    if ($proc) {
        # Kill the whole tree: the Viewer may spawn helpers.
        & taskkill.exe /T /F /PID $proc.Id *> $null
    }
    if (-not $KeepEnv) {
        if ($null -ne $savedDataDir) { $env:FASTLOOK_DATA_DIR = $savedDataDir }
        else { Remove-Item Env:\FASTLOOK_DATA_DIR -ErrorAction SilentlyContinue }
    }
}

Write-Host ("RESULT alive={0} window={1} responding={2} privateWS_MB={3} png={4}" -f $alive, $window, $responding, $ws, $(if ($window) { $Out } else { 'none' }))
if (Test-Path $perfLog) {
    Write-Host '--- perf log ---'
    Get-Content $perfLog | ForEach-Object { Write-Host $_ }
}
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue

if (-not $alive) { exit 1 }
if ($Exe -eq 'Viewer' -and -not $window) { exit 1 }
exit 0
