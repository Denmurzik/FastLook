#requires -Version 7
<#
.SYNOPSIS
  Server-mode check of the Private Bytes safety net (task 34): above FASTLOOK_MEMORY_CEILING_MB the Viewer exits at the next quiet
  moment, through the idle-exit path, after the ordered media close has run.
.DESCRIPTION
  Cases: a text file and a video with the ceiling at 1 MB (the Viewer must log the reason and exit with code 0 within 6 s of the hide,
  without a crash report), and a text file with the ceiling at 4000 MB (the Viewer must stay). Each case starts its own server Viewer
  with an isolated FASTLOOK_DATA_DIR and drives it over its pipe (show, then toggle to hide). Takes %TEMP%\gui-e2e.lock (the preview
  window shows and takes the foreground). Everything it launches is killed in `finally`. Exit code 1 when any check failed.
#>
[CmdletBinding()]
param([string]$Repo = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\GuiLock.ps1')
$exe = Join-Path $Repo 'out\bin\Release\FastLook.Viewer.exe'
if (-not (Test-Path $exe)) { throw "Not built: $exe (run scripts/build.ps1 first)" }
$send = Join-Path $PSScriptRoot 'ipc-send.ps1'
$text = (Resolve-Path (Join-Path $Repo 'tests\fixtures\shell\a.txt')).Path
$video = (Resolve-Path (Join-Path $Repo 'tests\fixtures\media\clip.mp4')).Path
$results = [ordered]@{}
function Check($name, $ok, $detail = '') { $script:results[$name] = [bool]$ok; Write-Host ("{0,-4} {1} {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail) }
function Msg($type, $id, $extra = @{}) { (@{ v = 1; id = $id; type = $type } + $extra) | ConvertTo-Json -Compress -Depth 6 }
function Send($pipe, $json) {
    $out = & pwsh -NoProfile -File $send -Pipe $pipe -Json $json -TimeoutMs 3000 2>&1
    if ($LASTEXITCODE -eq 0) { $out | ConvertFrom-Json } else { Write-Host "send failed code=$LASTEXITCODE out=$out"; $null }
}
function Crashes($since) {
    @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $since } -ErrorAction SilentlyContinue |
        Where-Object { $_.TimeCreated -ge $since -and $_.Message -like "*$exe*" }) # StartTime alone is not reliable here
}

# One server Viewer: show $file, hide it, then wait up to $waitMs for the process to end. Returns @{ exited; code; log }.
function Run-Case([string]$file, [int]$ceilingMB, [int]$shownMs, [int]$waitMs) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("fl-ceiling-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $proc = $null
    try {
        $env:FASTLOOK_DATA_DIR = $tmp; $env:FASTLOOK_IDLE_SECONDS = '600'; $env:FASTLOOK_MEMORY_CEILING_MB = "$ceilingMB"
        $pipe = "\\.\pipe\FastLook-test-$([guid]::NewGuid().ToString('N').Substring(0,8))"
        $proc = Start-Process $exe -ArgumentList @('--background', '--pipe', $pipe) -PassThru
        $null = $proc.Handle
        Start-Sleep -Seconds 3
        $item = @{ items = [object[]]@(@{ path = $file; virtual = $false }); index = 0; navMode = 'selection'; listComplete = $true; explorerHwnd = 0; isDesktop = $false; monitor = 0 }
        $shown = Send $pipe (Msg 'toggle' 1 $item)
        Start-Sleep -Milliseconds $shownMs
        $alive = -not $proc.HasExited
        $hidden = Send $pipe (Msg 'toggle' 2 $item)
        $exited = $proc.WaitForExit($waitMs)
        $log = @(Get-ChildItem (Join-Path $tmp 'logs') -Filter *.log -ErrorAction SilentlyContinue | ForEach-Object { Get-Content $_.FullName })
        [pscustomobject]@{
            shown = ($shown -and $shown.shown -eq $true); aliveWhileShown = $alive; hidden = ($hidden -and $hidden.shown -eq $false)
            exited = $exited; code = $(if ($exited) { $proc.ExitCode } else { $null }); log = $log
        }
    }
    finally {
        if ($proc -and -not $proc.HasExited) { & taskkill.exe /T /F /PID $proc.Id *> $null }
        Remove-Item Env:\FASTLOOK_MEMORY_CEILING_MB, Env:\FASTLOOK_IDLE_SECONDS, Env:\FASTLOOK_DATA_DIR -ErrorAction SilentlyContinue
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$start = Get-Date
try {
    Enter-GuiLock 'fastlook-test-memory-ceiling'
    foreach ($case in @(@{ name = 'text'; file = $text; shownMs = 1200 }, @{ name = 'video'; file = $video; shownMs = 2000 })) {
        $r = Run-Case $case.file 1 $case.shownMs 6000
        Check "ceiling 1 MB, $($case.name): shown and hidden" ($r.shown -and $r.aliveWhileShown -and $r.hidden)
        Check "ceiling 1 MB, $($case.name): exits with code 0 within 6 s of the hide" ($r.exited -and $r.code -eq 0) "exited=$($r.exited) code=$($r.code)"
        Check "ceiling 1 MB, $($case.name): log names the ceiling" (@($r.log | Where-Object { $_ -match 'above the 1 MB ceiling; exiting when quiet' }).Count -eq 1)
        Check "ceiling 1 MB, $($case.name): no media close failure in the log" (@($r.log | Where-Object { $_ -match 'Closing the media (player|source) failed' }).Count -eq 0)
    }
    $r = Run-Case $text 4000 1200 3000
    Check 'ceiling 4000 MB: the Viewer stays after the hide' ($r.hidden -and -not $r.exited)
    Start-Sleep -Seconds 2 # Windows Error Reporting writes its event a moment after a crash
    $crashes = Crashes $start
    Check 'no Viewer crash report during the run' ($crashes.Count -eq 0) $(if ($crashes.Count) { $crashes[0].Message.Split("`n")[0..2] -join ' | ' })
}
finally {
    Exit-GuiLock
}

$failed = @($results.Keys | Where-Object { -not $results[$_] })
Write-Host ("SUMMARY: {0} ({1}/{2})" -f $(if ($failed.Count) { 'FAIL' } else { 'PASS' }), ($results.Count - $failed.Count), $results.Count)
exit $(if ($failed.Count) { 1 } else { 0 })
