#requires -Version 7
<#
.SYNOPSIS
  Server-mode checks of FastLook.Viewer.exe: prewarm without focus theft, show/toggle/ping/items over the pipe,
  working-set trim, idle exit, single instance, shutdown and the hung-UI-thread check. Prints PASS/FAIL lines and a SUMMARY.
.DESCRIPTION
  Needs a built Release Viewer and a free desktop (it shows windows and reads the foreground window). Always kills what it launches.
  Exit code 1 when any check failed.
#>
param([string]$Repo = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
Set-Location $Repo
Import-Module (Join-Path $Repo 'scripts\lib\Win32.psm1') -Force
Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class Fg { [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow(); [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid); }'

$exe = Join-Path $Repo 'out\bin\Release\FastLook.Viewer.exe'
$send = Join-Path $Repo 'scripts\ipc-send.ps1'
$a = (Resolve-Path 'tests\fixtures\shell\a.txt').Path
$b = (Resolve-Path 'tests\fixtures\shell\b.txt').Path
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("fl25-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null
$results = [ordered]@{}
function Check($name, $ok, $detail = '') { $script:results[$name] = $ok; Write-Host ("{0,-4} {1} {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail) }
function Msg($type, $id, $extra = @{}) { (@{ v = 1; id = $id; type = $type } + $extra) | ConvertTo-Json -Compress -Depth 6 }
function Items($paths) { , @($paths | ForEach-Object { @{ path = $_; virtual = $false } }) }
function Send($pipe, $json, $timeout = 2000) { $out = & pwsh -NoProfile -File $send -Pipe $pipe -Json $json -TimeoutMs $timeout 2>&1; if ($LASTEXITCODE -eq 0) { $out | ConvertFrom-Json } else { Write-Host "send failed code=$LASTEXITCODE out=$out json=$json"; $null } }
function Visible($procId) { try { Wait-TopWindow -ProcessId $procId -TimeoutSec 1 } catch { $null } }

$proc = $null; $proc2 = $null
$env:FASTLOOK_DATA_DIR = Join-Path $tmp 'data'
try {
    # ---------- phase A: prewarm, show/toggle/ping/items, idle exit ----------
    $pipe = "\\.\pipe\FastLook-test-$([guid]::NewGuid().ToString('N').Substring(0,8))"
    $env:FASTLOOK_IDLE_SECONDS = '8'
    $fgBefore = [Fg]::GetForegroundWindow()
    $proc = Start-Process $exe -ArgumentList @('--background', '--pipe', $pipe, '--perf-log', (Join-Path $tmp 'perf.log')) -PassThru
    $null = $proc.Handle
    Start-Sleep -Seconds 3
    $fgAfter = [Fg]::GetForegroundWindow()
    Check 'prewarm: foreground unchanged' ($fgBefore -eq $fgAfter) "$fgBefore -> $fgAfter"
    $vis = Visible $proc.Id
    Check 'prewarm: no visible window' ($null -eq $vis)
    $ws = Get-PrivateWorkingSetMB -ProcessId $proc.Id
    Check 'prewarm: private WS <= 20 MB' ($ws -le 20) ('{0:n1} MB' -f $ws)

    $show = Send $pipe (Msg 'show' 11 @{ items = (Items @($a)); index = 0; navMode = 'selection'; listComplete = $true; explorerHwnd = 0; isDesktop = $false; monitor = 0 })
    Check 'show: ack shown:true re=11' ($show -and $show.type -eq 'ack' -and $show.shown -eq $true -and $show.re -eq 11) ($show | ConvertTo-Json -Compress)
    Start-Sleep -Milliseconds 800
    $hwnd = Visible $proc.Id
    Check 'show: window visible' ($null -ne $hwnd)
    if ($hwnd) { $null = Save-WindowPng -Hwnd $hwnd -Out (Join-Path $Repo 'out\shots\task25-show.png'); Write-Host "shot: out\shots\task25-show.png" }
    $fgNow = [Fg]::GetForegroundWindow(); $fgPid = 0; [void][Fg]::GetWindowThreadProcessId($fgNow, [ref]$fgPid)
    Check 'show: window is foreground' ($hwnd -and $fgNow -eq $hwnd) "hwnd=$hwnd fg=$fgNow fgPid=$fgPid viewerPid=$($proc.Id)"

    $ping = Send $pipe (Msg 'ping' 12)
    Check 'ping: pong re=12' ($ping -and $ping.type -eq 'pong' -and $ping.re -eq 12) ($ping | ConvertTo-Json -Compress)
    $items = Send $pipe (Msg 'items' 13 @{ items = (Items @($a, $b)); index = 0; navMode = 'folder' })
    Check 'items: ack re=13' ($items -and $items.type -eq 'ack' -and $items.re -eq 13) ($items | ConvertTo-Json -Compress)
    $toggle = Send $pipe (Msg 'toggle' 14 @{ items = (Items @($a)); index = 0; navMode = 'selection'; listComplete = $true; explorerHwnd = 0; isDesktop = $false; monitor = 0 })
    Check 'toggle: ack shown:false re=14' ($toggle -and $toggle.type -eq 'ack' -and $toggle.shown -eq $false -and $toggle.re -eq 14) ($toggle | ConvertTo-Json -Compress)
    Start-Sleep -Seconds 2
    Check 'toggle: window hidden' ($null -eq (Visible $proc.Id))
    $ws = Get-PrivateWorkingSetMB -ProcessId $proc.Id
    Check 'hidden 2 s: private WS <= 20 MB' ($ws -le 20) ('{0:n1} MB' -f $ws)

    $done = $proc.WaitForExit(14000)
    Check 'idle exit: process ended on its own' $done
    if ($done) { Check 'idle exit: exit code 0' ($proc.ExitCode -eq 0) "code=$($proc.ExitCode)" }
    if (Test-Path (Join-Path $tmp 'perf.log')) { Get-Content (Join-Path $tmp 'perf.log') | Where-Object { $_ -match '^(prewarm_done|trim|hidden)' } | ForEach-Object { Write-Host "perf: $_" } }

    # ---------- phase B: second instance refused (exit code 2) ----------
    $env:FASTLOOK_IDLE_SECONDS = '30'
    $pipeB = "\\.\pipe\FastLook-test-$([guid]::NewGuid().ToString('N').Substring(0,8))"
    $p1 = Start-Process $exe -ArgumentList @('--background', '--pipe', $pipeB) -PassThru
    Start-Sleep -Seconds 2
    $p2 = Start-Process $exe -ArgumentList @('--background', '--pipe', "$pipeB-2") -PassThru
    $null = $p2.Handle
    $ended = $p2.WaitForExit(8000)
    Check 'second instance: exit code 2' ($ended -and $p2.ExitCode -eq 2) "code=$($p2.ExitCode)"
    # shutdown -> no ack, process ends
    $sd = Send $pipeB (Msg 'shutdown' 21) 1000
    Check 'shutdown: no ack' ($null -eq $sd)
    $null = $p1.Handle
    Check 'shutdown: process exits within 4 s' ($p1.WaitForExit(4000)) "code=$($p1.ExitCode)"

    # ---------- phase C: hung UI thread ----------
    $env:FASTLOOK_TEST_UI_STALL_MS = '3000'
    $pipeC = "\\.\pipe\FastLook-test-$([guid]::NewGuid().ToString('N').Substring(0,8))"
    $proc2 = Start-Process $exe -ArgumentList @('--background', '--pipe', $pipeC) -PassThru
    Start-Sleep -Seconds 3
    $name = $pipeC -replace '^\\\\\.\\pipe\\', ''
    $c = [System.IO.Pipes.NamedPipeClientStream]::new('.', $name, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::Asynchronous)
    $c.Connect(3000)
    function Write-Frame($s, $json) { $p = [Text.Encoding]::UTF8.GetBytes($json); $f = [byte[]]::new(4 + $p.Length); [BitConverter]::GetBytes([uint32]$p.Length).CopyTo($f, 0); $p.CopyTo($f, 4); $s.Write($f, 0, $f.Length); $s.Flush() }
    function Read-Frame($s, $ms) {
        $cts = [Threading.CancellationTokenSource]::new($ms)
        try {
            $h = [byte[]]::new(4); $o = 0
            while ($o -lt 4) { $n = $s.ReadAsync($h, $o, 4 - $o, $cts.Token).GetAwaiter().GetResult(); if ($n -le 0) { return $null }; $o += $n }
            $len = [BitConverter]::ToUInt32($h, 0); $buf = [byte[]]::new($len); $o = 0
            while ($o -lt $len) { $n = $s.ReadAsync($buf, $o, $len - $o, $cts.Token).GetAwaiter().GetResult(); if ($n -le 0) { return $null }; $o += $n }
            [Text.Encoding]::UTF8.GetString($buf) | ConvertFrom-Json
        } catch { $null }
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Write-Frame $c (Msg 'show' 31 @{ items = (Items @($b)); index = 0; navMode = 'selection'; listComplete = $true; explorerHwnd = 0; isDesktop = $false; monitor = 0 })
    Start-Sleep -Milliseconds 200
    Write-Frame $c (Msg 'ping' 32)
    $r = Read-Frame $c 1500
    Check 'hung UI: ping gets no reply within 1500 ms' ($null -eq $r) ($r | ConvertTo-Json -Compress)
    $r1 = Read-Frame $c 6000
    $elapsed = $sw.ElapsedMilliseconds
    Check 'hung UI: show ack arrives after the stall' ($r1 -and $r1.type -eq 'ack' -and $r1.re -eq 31 -and $elapsed -ge 2900) "elapsed=${elapsed}ms"
    $r2 = Read-Frame $c 3000
    Check 'hung UI: pong follows' ($r2 -and $r2.type -eq 'pong' -and $r2.re -eq 32)
    $c.Dispose()
}
finally {
    foreach ($p in @($proc, $p1, $p2, $proc2)) { if ($p -and -not $p.HasExited) { & taskkill.exe /T /F /PID $p.Id *> $null } }
    Remove-Item Env:\FASTLOOK_IDLE_SECONDS, Env:\FASTLOOK_TEST_UI_STALL_MS, Env:\FASTLOOK_DATA_DIR -ErrorAction SilentlyContinue
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
$failed = @($results.GetEnumerator() | Where-Object { -not $_.Value })
Write-Host ("SUMMARY: {0}/{1} passed" -f ($results.Count - $failed.Count), $results.Count)
if ($failed.Count) { exit 1 }
