#requires -Version 7
<#
.SYNOPSIS
  Measures FastLook memory in three modes: visible, hidden (--hidden), hidden-trim (--hidden-trim).
.DESCRIPTION
  After a settle period, samples the perf counters "\Process(FastLook)\Working Set - Private" and
  "\Process(FastLook)\Private Bytes". Counters are read through PdhAddEnglishCounter, so the script works on
  localized Windows (Get-Counter needs localized counter paths). Hidden modes also re-show the window
  (--reshow-ms) and record time from AppWindow.Show() to the first rendered frame (--log).
  Reported value per mode is the median over -Runs launches.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [int]$SettleSeconds = 5,
    [int]$Samples = 3,
    [int]$Runs = 3,
    [int]$ReshowAfterMs = 12000
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "out\bin\$Configuration\FastLook.exe"
if (-not (Test-Path $exe)) { throw "Not built: $exe (run scripts/build.ps1 first)" }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Pdh
{
    [StructLayout(LayoutKind.Explicit)]
    public struct FmtValue
    {
        [FieldOffset(0)] public uint CStatus;
        [FieldOffset(8)] public long Large;
    }
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] public static extern uint PdhOpenQuery(string src, IntPtr user, out IntPtr q);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] public static extern uint PdhAddEnglishCounterW(IntPtr q, string path, IntPtr user, out IntPtr c);
    [DllImport("pdh.dll")] public static extern uint PdhCollectQueryData(IntPtr q);
    [DllImport("pdh.dll")] public static extern uint PdhGetFormattedCounterValue(IntPtr c, uint fmt, out uint type, out FmtValue v);
    [DllImport("pdh.dll")] public static extern uint PdhCloseQuery(IntPtr q);

    public static long Read(string path)
    {
        IntPtr q, c; uint type; FmtValue v;
        uint r = PdhOpenQuery(null, IntPtr.Zero, out q);
        if (r != 0) throw new Exception("PdhOpenQuery 0x" + r.ToString("X"));
        try
        {
            r = PdhAddEnglishCounterW(q, path, IntPtr.Zero, out c);
            if (r != 0) throw new Exception("PdhAddEnglishCounterW(" + path + ") 0x" + r.ToString("X"));
            PdhCollectQueryData(q);
            r = PdhGetFormattedCounterValue(c, 0x400 /* PDH_FMT_LARGE */, out type, out v);
            if (r != 0) throw new Exception("PdhGetFormattedCounterValue 0x" + r.ToString("X"));
            return v.Large;
        }
        finally { PdhCloseQuery(q); }
    }
}
'@

function Get-Median([double[]]$v) {
    $s = $v | Sort-Object
    $n = $s.Count
    if ($n % 2) { $s[($n - 1) / 2] } else { ($s[$n / 2 - 1] + $s[$n / 2]) / 2 }
}

function Measure-Mode([string]$Name, [string[]]$ModeArgs, [bool]$Reshow) {
    $wsp = @(); $pb = @(); $reshowMs = @(); $afterReshow = @()
    for ($run = 1; $run -le $Runs; $run++) {
        Get-Process FastLook -ErrorAction SilentlyContinue | Stop-Process -Force
        $log = Join-Path ([IO.Path]::GetTempPath()) "fastlook-measure-$PID.log"
        Remove-Item $log -ErrorAction SilentlyContinue
        $argList = @($ModeArgs)
        if ($Reshow) { $argList += @('--reshow-ms', $ReshowAfterMs, '--log', $log) }
        $p = Start-Process $exe -ArgumentList $argList -PassThru
        try {
            Start-Sleep -Seconds $SettleSeconds
            if ($p.HasExited) { throw "FastLook exited early in mode '$Name' (exit $($p.ExitCode))" }
            $ws = @(); $pv = @()
            for ($i = 0; $i -lt $Samples; $i++) {
                $ws += [Pdh]::Read('\Process(FastLook)\Working Set - Private')
                $pv += [Pdh]::Read('\Process(FastLook)\Private Bytes')
                Start-Sleep -Milliseconds 500
            }
            $wsp += (Get-Median $ws) / 1MB
            $pb += (Get-Median $pv) / 1MB

            if ($Reshow) {
                $deadline = (Get-Date).AddMilliseconds($ReshowAfterMs + 5000)
                while ((Get-Date) -lt $deadline -and -not ((Test-Path $log) -and (Select-String -Path $log -Pattern 'reshow_first_frame_ms' -Quiet))) {
                    Start-Sleep -Milliseconds 200
                }
                $m = Select-String -Path $log -Pattern 'reshow_first_frame_ms=([\d.]+)' -ErrorAction SilentlyContinue
                if ($m) { $reshowMs += [double]$m.Matches[0].Groups[1].Value }
                Start-Sleep -Milliseconds 1500
                $afterReshow += [Pdh]::Read('\Process(FastLook)\Working Set - Private') / 1MB
            }
        }
        finally {
            if (-not $p.HasExited) { $p | Stop-Process -Force }
            Remove-Item $log -ErrorAction SilentlyContinue
        }
    }
    [pscustomobject]@{
        Mode              = $Name
        PrivateWS_MB      = [math]::Round((Get-Median $wsp), 1)
        PrivateBytes_MB   = [math]::Round((Get-Median $pb), 1)
        ReshowFirstFrame  = if ($reshowMs.Count) { [math]::Round((Get-Median $reshowMs), 1) } else { '-' }
        PrivateWSAfterShow_MB = if ($afterReshow.Count) { [math]::Round((Get-Median $afterReshow), 1) } else { '-' }
        PrivateWS_runs_MB = ($wsp | ForEach-Object { [math]::Round($_, 1) }) -join ' / '
    }
}

$results = @(
    Measure-Mode 'visible'     @()                  $false
    Measure-Mode 'hidden'      @('--hidden')        $true
    Measure-Mode 'hidden-trim' @('--hidden-trim')   $true
)

Write-Host ''
Write-Host ("FastLook memory, {0}, settle {1}s, median of {2} runs ({3} samples each)" -f $Configuration, $SettleSeconds, $Runs, $Samples)
$results | Format-Table Mode, PrivateWS_MB, PrivateBytes_MB, @{ n = 'ReshowFirstFrame_ms'; e = { $_.ReshowFirstFrame } }, PrivateWSAfterShow_MB, PrivateWS_runs_MB -AutoSize
