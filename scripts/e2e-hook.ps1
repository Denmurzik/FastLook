#requires -Version 7
<#
.SYNOPSIS
  End-to-end test of the Space-bar hook through the real Windows Explorer (Task 31). Measures and reports; it fixes nothing.
.DESCRIPTION
  WARNING: this script TAKES KEYBOARD FOCUS and injects real key presses (SendInput) into the foreground window for roughly
  30-60 s with the default -Iterations 20 (about 7 min with -Iterations 1000). Do not type, click or switch windows while it
  runs. Run it only through the test lock (`<TL> pwsh -NoProfile -File scripts/e2e-hook.ps1`) and only after the user has agreed
  to a focus-stealing run.

  It opens its own Explorer windows (Shell.Application), starts a FastLook daemon with a temp FASTLOOK_DATA_DIR, drives it with
  Space/Right/Esc/F2/..., prints one `PASS|FAIL|SKIP <check> <details>` line per check and writes a JSON summary. `finally`
  closes every Explorer window it opened, runs `--shutdown`, kills every FastLook* process and deletes its temp files.
  PowerToys Peek is never touched; when it runs the script only warns and counts windows it opens.

  Latency is measured with QPC: sendQpc (just before SendInput of Space) to the first poll that sees a visible, unowned
  top-level window of FastLook.Viewer.exe (polling resolution ~0.1-1 ms). The daemon forwards its --perf-log to the Viewer as
  "<perf.log>.viewer", so the Viewer's own window_visible / content_ready marks are read from there (same QPC clock) and give
  the real Space -> content latency (warmContentMs); the daemon's hook_trigger / trigger_received / toggle_acked marks are
  reported as a breakdown. Window-visible polling resolution is a few ms (EnumWindows); the Viewer pid is re-resolved inside
  the polling loop.

  Safety: every injected key is guarded: the foreground must be the test Explorer window (focus in the item list for
  Space/F2/..., unless the check is about an edit box), the Viewer, or an explicitly allowed window. Otherwise the key is
  NOT sent, the section's checks become SKIP and a BLOCKED note is recorded. The script holds %TEMP%\gui-e2e.lock
  (waits until absent, creates it, deletes it in `finally`) so other GUI suites wait. `TaskbarCreated` is broadcast to all
  top-level windows to force a rehook (every tray app re-adds its icon).
.PARAMETER Iterations  Warm open/close cycles (default 20).
.PARAMETER Cold        Measure only the cold start (prewarm off, 5 runs, the Viewer is killed between runs).
.PARAMETER OutJson     Where the JSON summary goes (default out/e2e.json).
#>
[CmdletBinding()]
param(
    [int]$Iterations = 20,
    [switch]$Cold,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$OutJson = 'out/e2e.json'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
Import-Module (Join-Path $PSScriptRoot 'lib\Win32.psm1') -Force

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class E2E
{
    [DllImport("user32.dll")] public static extern uint RegisterWindowMessageW([MarshalAs(UnmanagedType.LPWStr)] string s);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetKeyboardLayout(uint tid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr LoadKeyboardLayoutW(string klid, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    public static IntPtr Broadcast(string name)
    {
        uint m = RegisterWindowMessageW(name);
        PostMessageW(new IntPtr(0xFFFF), m, IntPtr.Zero, IntPtr.Zero);
        return new IntPtr(m);
    }
    public static uint Tid(IntPtr h) { uint p; return GetWindowThreadProcessId(h, out p); }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] static extern IntPtr SetFocus(IntPtr h);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();

    // What a mouse click on a desktop icon leaves behind: keyboard focus in the SysListView32 (not in SHELLDLL_DefView itself).
    public static string FocusDesktopList(IntPtr progman)
    {
        IntPtr defView = FindWindowExW(progman, IntPtr.Zero, "SHELLDLL_DefView", null);
        if (defView == IntPtr.Zero)
        {
            IntPtr w = IntPtr.Zero;
            while ((w = FindWindowExW(IntPtr.Zero, w, "WorkerW", null)) != IntPtr.Zero)
            {
                defView = FindWindowExW(w, IntPtr.Zero, "SHELLDLL_DefView", null);
                if (defView != IntPtr.Zero) break;
            }
        }
        if (defView == IntPtr.Zero) return "no SHELLDLL_DefView";
        IntPtr list = FindWindowExW(defView, IntPtr.Zero, "SysListView32", null);
        if (list == IntPtr.Zero) return "no SysListView32";
        uint target = GetWindowThreadProcessId(list, out uint pid);
        uint me = GetCurrentThreadId();
        AttachThreadInput(me, target, true);
        SetFocus(list);
        AttachThreadInput(me, target, false);
        return "ok";
    }

    [StructLayout(LayoutKind.Sequential)]
    struct GUITHREADINFO
    {
        public int cbSize, flags;
        public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
        public int l, t, r, b;
    }
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO i);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    // The window that has keyboard focus in the foreground thread (the control, not the frame).
    public static IntPtr FocusHwnd()
    {
        IntPtr fg = GetForegroundWindow();
        if (fg == IntPtr.Zero) return IntPtr.Zero;
        uint pid; uint tid = GetWindowThreadProcessId(fg, out pid);
        var gi = new GUITHREADINFO(); gi.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
        return GetGUIThreadInfo(tid, ref gi) ? gi.hwndFocus : IntPtr.Zero;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder sb, int n);

    static string Cls(IntPtr h) { var sb = new System.Text.StringBuilder(128); if (h == IntPtr.Zero) return "-"; GetClassNameW(h, sb, 128); return sb.ToString(); }
    // "hwnd class 'title' pid:name focus=<class of the focused control in that thread>"
    public static string Describe(IntPtr h)
    {
        if (h == IntPtr.Zero) return "0";
        uint pid; uint tid = GetWindowThreadProcessId(h, out pid);
        string name = "?";
        try { using (var p = System.Diagnostics.Process.GetProcessById((int)pid)) name = p.ProcessName; } catch { }
        var sb = new System.Text.StringBuilder(128); GetWindowTextW(h, sb, 128);
        var gi = new GUITHREADINFO(); gi.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
        string focus = GetGUIThreadInfo(tid, ref gi) ? Cls(gi.hwndFocus) : "?";
        return h.ToInt64() + " " + Cls(h) + " '" + sb + "' " + pid + ":" + name + " focus=" + focus;
    }
}
'@

$VK = @{ Space = 0x20; Right = 0x27; Left = 0x25; Esc = 0x1B; F2 = 0x71; Ctrl = 0xA2; Shift = 0xA0; Alt = 0xA4; Enter = 0x0D }

$exe = Join-Path $repo "out\bin\$Configuration\FastLook.exe"
$viewerExe = Join-Path $repo "out\bin\$Configuration\FastLook.Viewer.exe"
foreach ($f in $exe, $viewerExe) { if (-not (Test-Path $f)) { throw "Not built: $f (run scripts/build.ps1 first)" } }
if (Get-Process -Name 'FastLook*' -ErrorAction SilentlyContinue) {
    throw 'FastLook* processes are running; stop them first (the daemon mutex is per session).'
}

# ---------- state ----------
$script:Deadline = [DateTime]::UtcNow.AddSeconds(300 + [Math]::Ceiling($Iterations * 1.5))
$script:Checks = [Collections.Generic.List[object]]::new()
$script:Failed = $false
$script:Missed = 0
$script:Crashes = 0
$script:PeekReacted = 0
$script:PeekWarned = $false
$script:Notes = [Collections.Generic.List[string]]::new()
$freq = [double][FastLookWin32]::QpcFreq()

$script:ExHwnd = [IntPtr]::Zero
$script:Allowed = @()
$script:Blocked = 0
$script:SectionBlocked = 0
$script:OrigHkl = $null
$script:OrigFocus = [IntPtr]::Zero
function Begin-Section { $script:SectionBlocked = $script:Blocked }
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    if ($script:PeekWarned) { Test-PeekWindow }
    if ($script:Blocked -gt $script:SectionBlocked) {
        Skip $Name ("a key was blocked because the foreground left the test window ({0} blocked in this section); would have been ok={1} {2}" -f ($script:Blocked - $script:SectionBlocked), $Ok, $Detail)
        return
    }
    $script:Checks.Add([pscustomobject]@{ name = $Name; pass = $Ok; skipped = $false; details = $Detail })
    if ($Ok) { Write-Host "PASS $Name $Detail" } else { Write-Host "FAIL $Name $Detail"; $script:Failed = $true }
}
function Skip([string]$Name, [string]$Detail) {
    $script:Checks.Add([pscustomobject]@{ name = $Name; pass = $null; skipped = $true; details = $Detail })
    Write-Host "SKIP $Name $Detail"
}
function Note([string]$Text) { $script:Notes.Add($Text); Write-Host "NOTE $Text" }
function Ms([long]$From, [long]$To) { ($To - $From) * 1000.0 / $freq }
function Pct([double[]]$Values, [double]$P) {
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    $s = @($Values | Sort-Object)
    $i = [Math]::Min($s.Count - 1, [Math]::Max(0, [int][Math]::Ceiling($P / 100.0 * $s.Count) - 1))
    [Math]::Round($s[$i], 1)
}
function Wait-Until([scriptblock]$Cond, [int]$TimeoutMs, [int]$PollMs = 5) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    do {
        if ([DateTime]::UtcNow -gt $script:Deadline) { throw 'e2e hard deadline exceeded' }
        if (& $Cond) { return $true }
        Start-Sleep -Milliseconds $PollMs
    } while ($sw.ElapsedMilliseconds -lt $TimeoutMs)
    $false
}
function Desc([IntPtr]$H) { [E2E]::Describe($H) }
function Pause([int]$Ms) { Start-Sleep -Milliseconds $Ms }
# $null = the foreground is an acceptable injection target; otherwise the reason. $List: focus must be in the item list.
function Get-InjectionBlocker([bool]$List) {
    $fg = Get-ForegroundHwnd
    if ($fg -eq [IntPtr]::Zero) { return 'no foreground window' }
    if ($script:ViewerPid -ne 0 -and $fg -eq [FastLookWin32]::FindTopWindow([uint32]$script:ViewerPid)) { return $null }
    if ($fg -eq $script:ExHwnd -or ($script:Allowed -contains [int64]$fg)) {
        if ($List) {
            $d = Desc $fg
            if ($d -notmatch 'focus=(DirectUIHWND|SysListView32)$') { return "focus is not in the item list [$d]" }
        }
        return $null
    }
    "foreground is another window [$(Desc $fg)]"
}
function Guard([string]$What, [bool]$List) {
    $why = Get-InjectionBlocker $List
    if (-not $why) { return $true }
    $script:Blocked++
    Note "BLOCKED $What : $why"
    $false
}
function Tap([int]$Vk, [switch]$List) { if (Guard "key 0x$($Vk.ToString('X'))" $List.IsPresent) { Send-Key -Vk $Vk } }
function Chord([int]$Mod, [int]$Vk, [switch]$List) {
    if (-not (Guard "chord 0x$($Mod.ToString('X'))+0x$($Vk.ToString('X'))" $List.IsPresent)) { return }
    Send-Key -Vk $Mod -Down; Send-Key -Vk $Vk; Send-Key -Vk $Mod -Up
}
# Sets the input layout of the focused control's thread; $true when that thread really has it afterwards.
function Ensure-Layout([string]$Klid) {
    if (-not (Guard "layout $Klid" $false)) { return $false }
    $f = [E2E]::FocusHwnd(); if ($f -eq [IntPtr]::Zero) { $f = $script:ExHwnd }
    $hkl = [E2E]::LoadKeyboardLayoutW($Klid, 0)
    foreach ($t in @($f, $script:ExHwnd)) { [E2E]::PostMessageW($t, 0x0050, [IntPtr]::Zero, $hkl) | Out-Null }   # WM_INPUTLANGCHANGEREQUEST
    Pause 400
    $now = [E2E]::GetKeyboardLayout([E2E]::Tid($f))
    $ok = (($now.ToInt64() -band 0xFFFF) -eq ($hkl.ToInt64() -band 0xFFFF))
    Note ("layout {0}: wanted 0x{1:X}, focus thread has 0x{2:X} -> {3}" -f $Klid, $hkl.ToInt64(), $now.ToInt64(), $ok)
    $ok
}

# ---------- Viewer window polling ----------
$script:ViewerPid = 0
function Refresh-ViewerPid {
    $p = [Diagnostics.Process]::GetProcessesByName('FastLook.Viewer')
    $script:ViewerPid = if ($p.Count) { $p[0].Id } else { 0 }
    foreach ($x in $p) { $x.Dispose() }
}
function Get-ViewerHwnd {
    if ($script:ViewerPid -ne 0) {
        $h = [FastLookWin32]::FindTopWindow([uint32]$script:ViewerPid)
        if ($h -ne [IntPtr]::Zero) { return $h }
    }
    Refresh-ViewerPid
    if ($script:ViewerPid -eq 0) { return [IntPtr]::Zero }
    [FastLookWin32]::FindTopWindow([uint32]$script:ViewerPid)
}
# Spins until a Viewer window is visible; returns @{ Qpc; Hwnd } or $null. Resolution: one EnumWindows pass (~0.2 ms).
function Wait-ViewerVisible([int]$TimeoutMs) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $n = 0
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        $h = if ($script:ViewerPid -ne 0) { [FastLookWin32]::FindTopWindow([uint32]$script:ViewerPid) } else { [IntPtr]::Zero }
        if ($h -ne [IntPtr]::Zero) { return @{ Qpc = [FastLookWin32]::QpcNow(); Hwnd = $h } }
        if ((++$n % 4) -eq 0 -or $script:ViewerPid -eq 0) { Refresh-ViewerPid }
        [Threading.Thread]::Yield() | Out-Null
    }
    $null
}
function Test-ViewerVisible { (Get-ViewerHwnd) -ne [IntPtr]::Zero }
function Wait-ViewerHidden([int]$TimeoutMs) { Wait-Until { -not (Test-ViewerVisible) } $TimeoutMs 2 }
# Returns the QPC just before the key, or $null when blocked. -Anywhere: the check is about an edit box (no list-focus requirement).
function Press-Space([switch]$Anywhere) {
    if (-not (Guard 'Space' (-not $Anywhere.IsPresent))) { return $null }
    $q = [FastLookWin32]::QpcNow(); Send-Key -Vk $VK.Space; $q
}

function Test-PeekWindow {
    $w = @(Find-Process-Windows -Name 'PowerToys.Peek.UI' | Where-Object { $_.Visible })
    if ($w.Count -gt 0) {
        $script:PeekReacted++
        foreach ($x in $w) { $script:Allowed = @([int64]$x.Hwnd); if (Set-ForegroundSafe -Hwnd $x.Hwnd -TimeoutMs 800) { Tap $VK.Esc } }
        $script:Allowed = @()
        Pause 200
    }
}

# ---------- paths ----------
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("fastlook-e2e-{0}" -f [guid]::NewGuid().ToString('N'))
$data = Join-Path $tmp 'data'
$folder = Join-Path $tmp 'folder'
$folder2 = Join-Path $tmp 'folder2'
New-Item -ItemType Directory -Force $data, $folder, $folder2 | Out-Null
$perfLog = Join-Path $tmp 'perf.log'
$logFile = Join-Path $data 'logs\daemon.log'
$shots = Join-Path $repo 'out\shots'
New-Item -ItemType Directory -Force $shots | Out-Null

$photo = Join-Path $repo 'out\fixtures\photo24mp.jpg'
$bigText = Join-Path $repo 'out\fixtures\text-10mb.txt'
$imgSrc = if (Test-Path $photo) { $photo } else { Join-Path $repo 'tests\fixtures\image\sample.png' }
$aName = 'a' + [IO.Path]::GetExtension($imgSrc)
Copy-Item $imgSrc (Join-Path $folder $aName)
if (Test-Path $bigText) { Copy-Item $bigText (Join-Path $folder 'b.txt') } else { Copy-Item (Join-Path $repo 'tests\fixtures\text\utf8.cpp') (Join-Path $folder 'b.txt') }
Copy-Item (Join-Path $repo 'tests\fixtures\markdown\readme.md') (Join-Path $folder 'c.md')
Set-Content (Join-Path $folder 'd.txt') 'rename target' -Encoding utf8
Set-Content (Join-Path $folder2 't1.txt') 'second tab' -Encoding utf8
Note ("fixtures: image={0} text={1}" -f (Split-Path $imgSrc -Leaf), $(if (Test-Path $bigText) { 'text-10mb.txt' } else { 'utf8.cpp (no large fixture present)' }))

$isElevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Note ("this script runs {0}; Explorer is the user's normal (medium) shell" -f $(if ($isElevated) { 'ELEVATED' } else { 'non-elevated' }))
if (Get-Process -Name 'ghostty' -ErrorAction SilentlyContinue) {
    Note 'WARN: ghostty.exe (another GUI test suite) is running; it steals the foreground and types keys, results of this run are unreliable'
}
if (Get-Process -Name 'PowerToys.Peek.UI' -ErrorAction SilentlyContinue) {
    $script:PeekWarned = $true
    Write-Host 'WARN PowerToys.Peek.UI is running: both Peek and FastLook may react to Space; Peek windows will be counted and closed.'
}

# ---------- Explorer COM ----------
$shell = New-Object -ComObject Shell.Application
$script:Opened = [Collections.Generic.List[object]]::new()   # @{ Hwnd; Shared }
function Get-CabinetHwnds { @(Find-Process-Windows -Name 'explorer' | Where-Object { $_.Class -eq 'CabinetWClass' } | ForEach-Object { [int64]$_.Hwnd }) }
function Local-Path([string]$Url) { try { [Uri]::new($Url).LocalPath.TrimEnd('\') } catch { '' } }
function Find-ShellWindow([string]$Path) {
    $want = $Path.TrimEnd('\')
    foreach ($w in $shell.Windows()) {
        try { if ((Local-Path $w.LocationURL) -ieq $want) { return $w } } catch { }
    }
    $null
}
function Open-Explorer([string]$Path) {
    $before = Get-CabinetHwnds
    $shell.Open($Path)
    $win = $null
    if (-not (Wait-Until { $script:found = Find-ShellWindow $Path; [bool]$script:found } 8000 100)) { return $null }
    $win = $script:found
    $hwnd = [IntPtr][int64]$win.HWND
    $shared = ($before -contains [int64]$hwnd)
    $script:Opened.Add(@{ Hwnd = $hwnd; Shared = $shared })
    if ($shared) {
        try { $win.Quit() } catch { }   # closes only the tab we just opened
        throw 'Shell.Application.Open landed in an existing Explorer window (tab); close your Explorer windows and re-run (the test never injects keys into a user window)'
    }
    $script:ExHwnd = $hwnd
    $win
}
function Explorer-Hwnd($win) { [IntPtr][int64]$win.HWND }
function Select-Item($win, [string]$Name) {
    $item = $win.Document.Folder.ParseName($Name)
    $win.Document.SelectItem($item, (1 -bor 4 -bor 8 -bor 16))
}
function Focused-Name($win) { try { $win.Document.FocusedItem.Name } catch { $null } }
function Focus-Explorer($win, [string]$Name) {
    Select-Item $win $Name
    $ok = Set-ForegroundSafe -Hwnd (Explorer-Hwnd $win) -TimeoutMs 2500
    Pause 120
    $ok
}
function Close-Explorer($entry) {
    $h = $entry.Hwnd
    try {
        foreach ($w in $shell.Windows()) {
            try { if ([int64]$w.HWND -eq [int64]$h -and -not $entry.Shared) { $w.Quit() } } catch { }
        }
    } catch { }
    if (-not $entry.Shared) {
        if (-not (Wait-Until { -not [FastLookWin32]::Exists($h) } 1500 50)) {
            [E2E]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
            Wait-Until { -not [FastLookWin32]::Exists($h) } 1500 50 | Out-Null
        }
    }
}

# ---------- daemon ----------
$saved = @{}
foreach ($n in 'FASTLOOK_DATA_DIR', 'FASTLOOK_IDLE_SECONDS') { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
function Write-Settings([bool]$Prewarm) {
    $obj = @{ version = 1; general = @{ prewarm = $Prewarm; desktopEnabled = $true; peekWarning = $false } }
    $obj | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $data 'settings.json') -Encoding utf8
}
$script:Daemon = $null
$viewerPerfLog = "$perfLog.viewer"
function Start-Daemon {
    Remove-Item $perfLog, $viewerPerfLog -ErrorAction SilentlyContinue
    $script:Daemon = Start-Process -FilePath $exe -ArgumentList @('--perf-log', $perfLog) -PassThru
    Start-Sleep -Milliseconds 700
}
# Marks the Viewer wrote after $FromQpc (a script QPC tick): window_visible and content_ready, in ms from $FromQpc (or $null).
function Get-ViewerMarkMs([long]$FromQpc, [int]$WaitContentMs = 0) {
    $end = [DateTime]::UtcNow.AddMilliseconds($WaitContentMs)
    do {
        $win = $null; $content = $null
        try {
            $copy = Join-Path $tmp 'viewer-perf-copy.log'
            $fs = [IO.File]::Open($viewerPerfLog, 'Open', 'Read', 'ReadWrite,Delete'); $o = [IO.File]::Create($copy); $fs.CopyTo($o); $o.Dispose(); $fs.Dispose()
            foreach ($m in (Read-PerfLog $copy)) {
                if ($m.qpc -le $FromQpc) { continue }
                if ($m.name -eq 'window_visible' -and $null -eq $win) { $win = Ms $FromQpc $m.qpc }
                elseif ($m.name -eq 'content_ready' -and $null -eq $content) { $content = Ms $FromQpc $m.qpc }
            }
        } catch { }
        if ($null -ne $content -or [DateTime]::UtcNow -ge $end) { break }
        Start-Sleep -Milliseconds 40
    } while ($true)
    [pscustomobject]@{ Window = $win; Content = $content }
}
function Stop-Daemon {
    try { $p = Start-Process -FilePath $exe -ArgumentList '--shutdown' -PassThru -WindowStyle Hidden; $null = $p.WaitForExit(4000) } catch { }
    if ($script:Daemon) { $null = $script:Daemon.WaitForExit(3000) }
    Get-Process -Name 'FastLook*' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
$script:DaemonDeathCounted = $false
function Count-DaemonDeath { if (-not $script:DaemonDeathCounted) { $script:DaemonDeathCounted = $true; $script:Crashes++ } }
function Daemon-Alive { $script:Daemon -and -not $script:Daemon.HasExited }
function LogText { if (Test-Path $logFile) { try { [IO.File]::ReadAllText($logFile) } catch { '' } } else { '' } }

# Close any Viewer window with Esc (Viewer takes focus first). Returns after it is hidden or gives up.
function Close-Preview {
    $h = Get-ViewerHwnd
    if ($h -eq [IntPtr]::Zero) { return }
    if (Set-ForegroundSafe -Hwnd $h -TimeoutMs 1200) { Tap $VK.Esc }
    Wait-ViewerHidden 1500 | Out-Null
}

$lockPath = Join-Path ([IO.Path]::GetTempPath()) 'gui-e2e.lock'
$script:LockHeld = $false
function Enter-GuiLock {
    $end = [DateTime]::UtcNow.AddMinutes(20)
    while ($true) {
        try {
            if ((Test-Path $lockPath) -and ((Get-Item $lockPath).LastWriteTimeUtc -lt [DateTime]::UtcNow.AddMinutes(-45))) { Remove-Item $lockPath -Force -ErrorAction SilentlyContinue }
            $fs = [IO.File]::Open($lockPath, 'CreateNew', 'Write', 'None')
            $b = [Text.Encoding]::UTF8.GetBytes("fastlook impl-32 e2e-hook $([DateTime]::UtcNow.ToString('o'))")
            $fs.Write($b, 0, $b.Length); $fs.Dispose()
            $script:LockHeld = $true
            return
        } catch [IO.IOException] {
            if ([DateTime]::UtcNow -gt $end) { throw "desktop lock $lockPath is held by someone else for 20 min" }
            Start-Sleep -Seconds 2
        }
    }
}

$explorerWin = $null
try {
    Enter-GuiLock
    $script:Deadline = [DateTime]::UtcNow.AddSeconds(300 + [Math]::Ceiling($Iterations * 1.5))
    Write-Host 'WARNING: e2e-hook takes keyboard focus; do not touch keyboard/mouse until it finishes.'
    $env:FASTLOOK_DATA_DIR = $data
    $env:FASTLOOK_IDLE_SECONDS = $null
    $fgStart = Get-ForegroundHwnd

    if ($Cold) {
        Write-Settings $false
        Start-Daemon
        $explorerWin = Open-Explorer $folder
        if (-not $explorerWin) { Check 'setup: Explorer window opened' $false; throw 'no Explorer window' }
        if (-not (Focus-Explorer $explorerWin $aName)) { Check 'setup: focus Explorer' $false 'could not focus Explorer'; throw 'no focus' }
        $coldRuns = [Collections.Generic.List[double]]::new()
        $coldMissed = 0
        for ($i = 1; $i -le 5; $i++) {
            Get-Process -Name 'FastLook.Viewer' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            $script:ViewerPid = 0
            Pause 2000
            $null = Focus-Explorer $explorerWin $aName
            Pause 150
            $t0 = Press-Space
            $v = if ($null -ne $t0) { Wait-ViewerVisible 3000 } else { $null }
            if ($v) { $coldRuns.Add((Ms $t0 $v.Qpc)); Write-Host ("cold run {0}: {1:N0} ms" -f $i, (Ms $t0 $v.Qpc)) } else { $coldMissed++ }
            Pause 500
            Close-Preview
        }
        $coldP50 = Pct $coldRuns.ToArray() 50
        Check 'cold: Space -> window (5 runs)' ($coldMissed -eq 0 -and $coldRuns.Count -gt 0) ("p50={0} ms max={1:N0} ms missed={2} (target <= 400 ms, measured by window polling)" -f $coldP50, ($coldRuns | Measure-Object -Maximum).Maximum, $coldMissed)
        $summary = [ordered]@{ coldWindowMs = [ordered]@{ p50 = $coldP50; p95 = (Pct $coldRuns.ToArray() 95); runs = $coldRuns.ToArray(); missed = $coldMissed } }
        $script:ColdSummary = $summary
    }
    else {
        Write-Settings $true
        Start-Daemon
        Check 'setup: daemon alive' (Daemon-Alive)
        $explorerWin = Open-Explorer $folder
        if (-not $explorerWin) { Check 'setup: Explorer window opened' $false; throw 'no Explorer window' }
        $exHwnd = Explorer-Hwnd $explorerWin

        # Remember the user's layout (of the focused control's thread); Latin checks switch to English, restored in finally.
        $script:OrigFocus = [E2E]::FocusHwnd()
        if ($script:OrigFocus -eq [IntPtr]::Zero) { $script:OrigFocus = $exHwnd }
        $script:OrigHkl = [E2E]::GetKeyboardLayout([E2E]::Tid($script:OrigFocus))
        Note ("original layout 0x{0:X}" -f $script:OrigHkl.ToInt64())

        Begin-Section
        # ---- prewarm does not steal focus ----
        if (-not (Focus-Explorer $explorerWin $aName)) { Check 'setup: focus Explorer' $false 'could not focus Explorer'; throw 'could not focus Explorer' }
        $fgBeforePrewarm = Get-ForegroundHwnd
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $pw = $false
        while ($sw.ElapsedMilliseconds -lt 6000 -and -not $pw) { Refresh-ViewerPid; $pw = $script:ViewerPid -ne 0; Pause 50 }
        if ($pw) { Pause 1500 }
        Check 'prewarm: hidden Viewer started after Explorer got focus' $pw $(if ($pw) { "(pid $script:ViewerPid after $($sw.ElapsedMilliseconds) ms)" } else { '(no Viewer within 6 s; later Space runs are then cold-ish)' })
        Check 'prewarm: foreground unchanged and no Viewer window visible' ((Get-ForegroundHwnd) -eq $fgBeforePrewarm -and -not (Test-ViewerVisible)) ("(fg now [{0}], was [{1}])" -f (Desc (Get-ForegroundHwnd)), (Desc $fgBeforePrewarm))

        Begin-Section
        # ---- 1. warm open / close ----
        $warm = [Collections.Generic.List[double]]::new()
        $imageContentMs = [Collections.Generic.List[double]]::new(); $textContentMs = [Collections.Generic.List[double]]::new()
        $imageMs = [Collections.Generic.List[double]]::new()
        $textMs = [Collections.Generic.List[double]]::new()
        $hideMs = [Collections.Generic.List[double]]::new()
        $noForeground = 0; $focusLost = 0; $notHidden = 0; $lastPid = 0; $blockedIters = 0; $aborted = $false
        $firstShot = $false
        for ($i = 1; $i -le $Iterations; $i++) {
            if (-not (Daemon-Alive)) { Count-DaemonDeath; Note "daemon died before iteration $i"; break }
            if (-not (Focus-Explorer $explorerWin $aName)) { Note "iteration ${i}: could not focus Explorer"; $aborted = $true; break }
            $t0 = Press-Space
            if ($null -eq $t0) { $blockedIters++; Pause 300; continue }
            $v = Wait-ViewerVisible 2000
            if (-not $v) {
                $script:Missed++
                Note ("TRACE iter {0}: no window in 2 s; fg=[{1}]; viewer windows: {2}" -f $i, (Desc (Get-ForegroundHwnd)), ((Find-Process-Windows -Name 'FastLook.Viewer' | ForEach-Object { "$($_.Hwnd):$($_.Class):vis=$($_.Visible)" }) -join ' '))
                Pause 300; continue
            }
            $ms = Ms $t0 $v.Qpc
            $warm.Add($ms); $imageMs.Add($ms)
            if ($script:ViewerPid -ne $lastPid -and $lastPid -ne 0) { $script:Crashes++ ; Note "Viewer pid changed $lastPid -> $script:ViewerPid at iteration $i" }
            $lastPid = $script:ViewerPid
            if ($script:PeekWarned) { Test-PeekWindow }
            $fgOk = Wait-Until { (Get-ForegroundHwnd) -eq $v.Hwnd } 700 5
            if (-not $fgOk) { $noForeground++ ; Note ("TRACE iter {0}: Viewer not foreground; fg=[{1}]" -f $i, (Desc (Get-ForegroundHwnd))); $null = Set-ForegroundSafe -Hwnd $v.Hwnd -TimeoutMs 800 }
            if (-not $firstShot) {
                Pause 450
                try { $m = Save-WindowPng -Hwnd $v.Hwnd -Out (Join-Path $shots 'e2e-warm.png'); Write-Host "shot: out\shots\e2e-warm.png ($m)" } catch { Write-Host "shot failed: $_" }
                $firstShot = $true
            }
            $mk = Get-ViewerMarkMs $t0 3000
            if ($null -ne $mk.Content) { $imageContentMs.Add($mk.Content) } else { Note "iteration ${i}: no content_ready mark within 3 s of window-visible" }
            Pause 120
            $h0 = Press-Space
            $hidden = Wait-ViewerHidden 1500
            if ($null -ne $h0 -and $hidden) { $hideMs.Add((Ms $h0 ([FastLookWin32]::QpcNow()))) }
            else { $notHidden++; Note ("TRACE iter {0}: second Space did not hide; fg=[{1}]" -f $i, (Desc (Get-ForegroundHwnd))); Close-Preview }
            if (-not (Wait-Until { (Get-ForegroundHwnd) -eq $exHwnd } 800 10)) { $focusLost++; Note ("TRACE iter {0}: focus not back in Explorer; fg=[{1}]" -f $i, (Desc (Get-ForegroundHwnd))) }
            Pause 150
        }
        $p50 = Pct $warm.ToArray() 50; $p95 = Pct $warm.ToArray() 95
        $accounted = ($warm.Count + $script:Missed + $blockedIters -eq $Iterations)
        Check 'warm: Space -> Viewer window <= 2 s, every iteration completed' ($script:Missed -eq 0 -and $warm.Count -eq $Iterations -and $accounted -and -not $aborted) ("iterations=$Iterations completed=$($warm.Count) missed=$script:Missed blocked=$blockedIters aborted=$aborted p50=$p50 ms p95=$p95 ms (target warm <= 50 ms)")
        Check 'warm: Viewer has foreground right after Space' ($noForeground -eq 0) "(not foreground within 700 ms in $noForeground of $($warm.Count) opens)"
        Check 'warm: second Space (Viewer foreground) hides it' ($notHidden -eq 0) ("(not hidden in $notHidden of $($warm.Count); hide p50={0} ms)" -f (Pct $hideMs.ToArray() 50))
        Check 'warm: focus back in Explorer after close' ($focusLost -eq 0) "(lost in $focusLost of $($warm.Count))"

        Begin-Section
        # ---- 2. content ----
        $textRuns = [Math]::Min($Iterations, 10)
        $textMissed = 0
        for ($i = 1; $i -le $textRuns; $i++) {
            $null = Focus-Explorer $explorerWin 'b.txt'
            $t0 = Press-Space
            $v = Wait-ViewerVisible 2500
            if ($v) {
                $textMs.Add((Ms $t0 $v.Qpc))
                $mk = Get-ViewerMarkMs $t0 4000
                if ($null -ne $mk.Content) { $textContentMs.Add($mk.Content) } else { Note "text run ${i}: no content_ready mark within 4 s" }
            } else { $textMissed++ }
            Pause 300
            Close-Preview
            $null = Wait-Until { (Get-ForegroundHwnd) -eq $exHwnd } 800 10
            Pause 150
        }
        $imgP50 = Pct $imageMs.ToArray() 50; $txtP50 = Pct $textMs.ToArray() 50
        $imgC50 = Pct $imageContentMs.ToArray() 50; $txtC50 = Pct $textContentMs.ToArray() 50
        Check 'content: Space -> content_ready mark (image / text)' ($imageContentMs.Count -gt 0 -and $textContentMs.Count -gt 0 -and $imageContentMs.Count -eq $warm.Count -and $textMissed -eq 0) ("(image p50={0} ms n={1}/{2}; text p50={3} ms n={4}/{5}; window-visible p50 image={6} ms text={7} ms; Viewer perf marks)" -f $imgC50, $imageContentMs.Count, $warm.Count, $txtC50, $textContentMs.Count, $textRuns, $imgP50, $txtP50)

        Begin-Section
        # ---- daemon perf-log breakdown ----
        try {
            $perfCopy = Join-Path $tmp 'perf-copy.log'
            $fs = [IO.File]::Open($perfLog, 'Open', 'Read', 'ReadWrite,Delete'); $out = [IO.File]::Create($perfCopy); $fs.CopyTo($out); $out.Dispose(); $fs.Dispose()
            $perf = @(Read-PerfLog $perfCopy)
            $trig = @($perf | Where-Object name -eq 'hook_trigger'); $recv = @($perf | Where-Object name -eq 'trigger_received'); $ack = @($perf | Where-Object name -eq 'toggle_acked')
            $hookToRecv = [Collections.Generic.List[double]]::new()
            foreach ($t in $trig) {
                if ($t.detail -match 'qpc_at_hook=(\d+)') { $hookToRecv.Add((($t.qpc - [int64]$Matches[1]) * 1000.0 / $freq)) }
            }
            Note ("daemon perf marks: hook_trigger={0} trigger_received={1} toggle_acked={2}; hook callback -> WM_FL_TRIGGER handled p50={3} ms p95={4} ms" -f $trig.Count, $recv.Count, $ack.Count, (Pct $hookToRecv.ToArray() 50), (Pct $hookToRecv.ToArray() 95))
        } catch { Note "perf log unreadable: $_" }

        Begin-Section
        # ---- 3. navigation + selection sync + Esc returns focus ----
        $null = Focus-Explorer $explorerWin $aName
        $t0 = Press-Space
        $v = Wait-ViewerVisible 2000
        if (-not $v) { Check 'navigation: Space on a opens' $false 'no window'; }
        else {
            $null = Wait-Until { (Get-ForegroundHwnd) -eq $v.Hwnd } 700 5
            $vh = $v.Hwnd
            Pause 400
            Tap $VK.Right
            $titleOk = Wait-Until { (Get-WindowTitle -Hwnd $vh) -like '*b.txt*' } 2000 20
            Check 'navigation: Right -> title shows b.txt' $titleOk ("(title='{0}')" -f (Get-WindowTitle -Hwnd $vh))
            $syncSw = [Diagnostics.Stopwatch]::StartNew()
            $syncOk = Wait-Until { (Focused-Name $explorerWin) -eq 'b.txt' } 1000 20
            Check 'navigation: Explorer FocusedItem == b.txt within 1 s' $syncOk ("(focused='{0}', {1} ms; spec 500 ms)" -f (Focused-Name $explorerWin), $syncSw.ElapsedMilliseconds)
            Pause 300
            Tap $VK.Right
            $t2 = Wait-Until { (Get-WindowTitle -Hwnd $vh) -like '*c.md*' } 2000 20
            $s2 = Wait-Until { (Focused-Name $explorerWin) -eq 'c.md' } 1000 20
            Check 'navigation: Right again -> c.md (title and Explorer selection)' ($t2 -and $s2) ("(title='{0}', focused='{1}')" -f (Get-WindowTitle -Hwnd $vh), (Focused-Name $explorerWin))
            Pause 300
            Tap $VK.Left; Pause 350; Tap $VK.Left
            $t3 = Wait-Until { (Get-WindowTitle -Hwnd $vh) -like "*$aName*" } 2000 20
            $s3 = Wait-Until { (Focused-Name $explorerWin) -eq $aName } 1000 20
            Check "navigation: Left twice -> $aName (title and Explorer selection)" ($t3 -and $s3) ("(title='{0}', focused='{1}')" -f (Get-WindowTitle -Hwnd $vh), (Focused-Name $explorerWin))
            Pause 300
            Tap $VK.Esc
            $hid = Wait-ViewerHidden 1500
            $fgBack = Wait-Until { (Get-ForegroundHwnd) -eq $exHwnd } 1200 10
            Check 'esc: closes the preview' $hid
            Check 'esc: focus returns to the same Explorer window' $fgBack ("(foreground={0}, class={1}, Explorer={2})" -f (Get-ForegroundHwnd), [FastLookWin32]::GetClass((Get-ForegroundHwnd)), $exHwnd)
        }
        Pause 300

        Begin-Section
        # ---- 4. rename: F2, Z, Space, Y, Enter ----
        $null = Focus-Explorer $explorerWin 'd.txt'
        $blockedBeforeF2 = $script:Blocked
        Tap $VK.F2 -List; Pause 500
        if ($script:Blocked -gt $blockedBeforeF2) {
            Skip 'rename: no Viewer window on Space in the rename box' 'F2 was blocked (foreground/focus not the Explorer list); the rest of the section was not sent'
            Skip 'rename: the Space reached the edit box' 'F2 was blocked; the rest of the section was not sent'
        }
        else {
            $renLayout = Ensure-Layout '00000409'
            Tap 0x5A; Pause 60
            $rt0 = Press-Space -Anywhere
            $rv = Wait-ViewerVisible 900
            Pause 60
            Tap 0x59; Pause 100
            Tap $VK.Enter; Pause 700
            $renamed = [bool](Get-ChildItem $folder -Filter '? ?.txt' -ErrorAction SilentlyContinue)
            $renamedTo = (Get-ChildItem $folder -Filter '? ?.txt' -ErrorAction SilentlyContinue | Select-Object -First 1).Name
            if ($rv) { Close-Preview; $null = Focus-Explorer $explorerWin $aName }
            Check 'rename: no Viewer window on Space in the rename box' (-not $rv)
            Check 'rename: the Space reached the edit box (d.txt became "<letter> <letter>.txt")' $renamed ("(English layout in the edit box: $renLayout; files: {0})" -f ((Get-ChildItem $folder).Name -join ', '))
            if ($renamed) { Rename-Item (Join-Path $folder $renamedTo) 'd.txt' }
        }

        Begin-Section
        # ---- 5. address bar ----
        $null = Focus-Explorer $explorerWin $aName
        Chord $VK.Alt 0x44 -List; Pause 500      # Alt+D
        $null = Press-Space -Anywhere
        $av = Wait-ViewerVisible 900
        Check 'address bar: no Viewer window on Space (Alt+D)' (-not $av)
        if ($av) { Close-Preview }
        Tap $VK.Esc; Pause 300

        Begin-Section
        # ---- 6. type-ahead (Latin) ----
        $null = Focus-Explorer $explorerWin $aName
        Pause 1300
        $enOk = Ensure-Layout '00000409'
        if (-not $enOk) {
            Skip 'type-ahead (Latin layout): B then Space within 300 ms -> no Viewer' 'could not switch the focused control to the English layout'
            Skip 'type-ahead (Latin layout): after 1.2 s Space opens the preview' 'could not switch the focused control to the English layout'
        }
        else {
            Tap 0x42 -List                      # B
            Pause 100
            $null = Press-Space
            $tv = Wait-ViewerVisible 900
            Check 'type-ahead (Latin layout): B then Space within 300 ms -> no Viewer' (-not $tv)
            if ($tv) { Close-Preview }
            Pause 1300
            $null = Press-Space
            $tv2 = Wait-ViewerVisible 2000
            Check 'type-ahead (Latin layout): after 1.2 s Space opens the preview' ($null -ne $tv2)
            if ($tv2) { Close-Preview; $null = Wait-Until { (Get-ForegroundHwnd) -eq $exHwnd } 800 10 }
        }

        Begin-Section
        # ---- 6b. type-ahead with the Russian layout (OEM key) ----
        $ruInstalled = $false
        try { $ruInstalled = [bool](Get-WinUserLanguageList | Where-Object { $_.LanguageTag -like 'ru*' }) } catch { }
        if (-not $ruInstalled) { Skip 'type-ahead (Russian layout): б then Space -> no Viewer' 'Russian keyboard layout not installed' }
        else {
            $null = Focus-Explorer $explorerWin $aName
            Pause 1300
            $switched = Ensure-Layout '00000419'
            if (-not $switched) { Skip 'type-ahead (Russian layout): б then Space -> no Viewer' 'could not switch the focused control to the Russian layout' }
            else {
                Tap 0xBC -List                  # VK_OEM_COMMA = 'б' on the Russian layout
                Pause 100
                $null = Press-Space
                $rv2 = Wait-ViewerVisible 900
                Check 'type-ahead (Russian layout): б then Space within 300 ms -> no Viewer' (-not $rv2)
                if ($rv2) { Close-Preview }
            }
        }

        Begin-Section
        # ---- 7. Win11 tabs ----
        $null = Focus-Explorer $explorerWin $aName
        $firstUrl = $explorerWin.LocationURL
        Chord $VK.Ctrl 0x54 -List; Pause 1500     # Ctrl+T
        $same = @()
        foreach ($w in $shell.Windows()) { try { if ([int64]$w.HWND -eq [int64]$exHwnd) { $same += $w } } catch { } }
        $newTab = $same | Where-Object { (Local-Path $_.LocationURL) -ine $folder.TrimEnd('\') } | Select-Object -First 1
        if ($same.Count -lt 2 -or -not $newTab) { Skip 'tabs: active tab is previewed' "could not create a second tab (windows with this HWND: $($same.Count))" }
        else {
            try {
                $newTab.Navigate2($folder2)
                $tabWin = $null
                $null = Wait-Until { $script:tw = Find-ShellWindow $folder2; [bool]$script:tw } 4000 100
                $tabWin = $script:tw
                if (-not $tabWin) { Skip 'tabs: active tab is previewed' 'second tab did not navigate' }
                else {
                    Pause 600
                    Select-Item $tabWin 't1.txt'
                    $null = Set-ForegroundSafe -Hwnd $exHwnd -TimeoutMs 2000
                    Pause 300
                    $null = Press-Space
                    $tv3 = Wait-ViewerVisible 2000
                    $t3title = if ($tv3) { Get-WindowTitle -Hwnd $tv3.Hwnd } else { '' }
                    Check 'tabs: Space in the second (active) tab previews its item (t1.txt)' ($tv3 -and $t3title -like '*t1.txt*') ("(title='$t3title')")
                    if ($tv3) { Close-Preview; $null = Wait-Until { (Get-ForegroundHwnd) -eq $exHwnd } 800 10 }
                    Chord $VK.Ctrl 0x31; Pause 700   # Ctrl+1: first tab
                    Select-Item $explorerWin $aName
                    Pause 300
                    $null = Press-Space
                    $tv4 = Wait-ViewerVisible 2000
                    $t4title = if ($tv4) { Get-WindowTitle -Hwnd $tv4.Hwnd } else { '' }
                    Check "tabs: after switching back Space previews the first tab's item ($aName)" ($tv4 -and $t4title -like "*$aName*") ("(title='$t4title')")
                    if ($tv4) { Close-Preview; $null = Wait-Until { (Get-ForegroundHwnd) -eq $exHwnd } 800 10 }
                }
            } catch { Skip 'tabs: active tab is previewed' "error: $_" }
            # Ctrl+W only when the second tab exists (it was created above) and Ctrl+2 activated it
            Chord $VK.Ctrl 0x32; Pause 600   # Ctrl+2: second tab
            $secondActive = $false
            foreach ($w in $shell.Windows()) { try { if ([int64]$w.HWND -eq [int64]$exHwnd -and (Local-Path $w.LocationURL) -ieq $folder2.TrimEnd('\')) { $secondActive = $true } } catch { } }
            if ($secondActive -and (Get-ForegroundHwnd) -eq $exHwnd -and (Get-WindowTitle -Hwnd $exHwnd) -like '*folder2*') { Chord $VK.Ctrl 0x57; Pause 700 }   # Ctrl+W: close it; the first tab stays
            else { Note 'tabs: second tab not found afterwards, Ctrl+W not sent (it is closed with the window in finally)' }
        }

        Begin-Section
        # ---- 8. search box (last Explorer-window check: focus is not restored afterwards) ----
        $null = Focus-Explorer $explorerWin $aName
        Chord $VK.Ctrl 0x45 -List; Pause 600      # Ctrl+E
        Tap 0x5A; Pause 60
        $null = Press-Space -Anywhere
        $sv = Wait-ViewerVisible 900
        Check 'search box: no Viewer window on Space (Ctrl+E)' (-not $sv)
        if ($sv) { Close-Preview }
        Tap $VK.Esc; Pause 200; Tap $VK.Esc; Pause 300

        Begin-Section
        # ---- 9. desktop ----
        $deskFile = Join-Path ([Environment]::GetFolderPath('Desktop')) ("fastlook-e2e-{0}.txt" -f [guid]::NewGuid().ToString('N').Substring(0, 8))
        try {
            Set-Content $deskFile 'desktop e2e' -Encoding utf8
            Pause 800
            $loc = 0; $root = $null; $dh = 0
            $disp = $shell.Windows().FindWindowSW([ref]$loc, [ref]$root, 8, [ref]$dh, 1)
            if (-not $disp) { Skip 'desktop: Space on a desktop item opens' 'desktop shell window not found' }
            else {
                $dn = Split-Path $deskFile -Leaf
                $item = $disp.Document.Folder.ParseName($dn)
                if (-not $item) { $item = $disp.Document.Folder.ParseName([IO.Path]::GetFileNameWithoutExtension($dn)) }
                if (-not $item) { Skip 'desktop: Space on a desktop item opens' 'item not visible on the desktop' }
                else {
                    $disp.Document.SelectItem($item, (1 -bor 4 -bor 8 -bor 16))
                    $dhwnd = [IntPtr][int64]$dh
                    $script:Allowed = @([int64]$dhwnd)
                    if (-not (Set-ForegroundSafe -Hwnd $dhwnd -TimeoutMs 2500)) { Skip 'desktop: Space on a desktop item opens' 'could not focus the desktop' }
                    else {
                        Pause 300
                        $fr = [E2E]::FocusDesktopList($dhwnd)
                        Note ("desktop: emulated icon click (focus into SysListView32): $fr; foreground now [{0}]" -f (Desc (Get-ForegroundHwnd)))
                        Pause 200
                        $null = Press-Space
                        $dv = Wait-ViewerVisible 2000
                        Check 'desktop: Space on a selected desktop item opens the preview' ($null -ne $dv) ("(foreground=[{0}] item='{1}')" -f (Desc (Get-ForegroundHwnd)), $dn)
                        if ($dv) { Close-Preview }
                    }
                }
            }
        } catch { Skip 'desktop: Space on a desktop item opens' "error: $_" }
        finally { $script:Allowed = @(); Remove-Item $deskFile -Force -ErrorAction SilentlyContinue }

        Begin-Section
        # ---- 10. hook callback time: force a rehook via TaskbarCreated to flush the stat ----
        function Read-CbStats { @([regex]::Matches((LogText), 'hook installed hook_cb_max_us=(\d+) hook_cb_p50_us=(\d+) hook_cb_p99_us=(\d+) hook_cb_count=(\d+) hook_space_max_us=(\d+) hook_space_p50_us=(\d+) hook_space_p99_us=(\d+) hook_space_count=(\d+)(?: hook_trigger_max_us=(\d+) hook_trigger_p50_us=(\d+) hook_trigger_count=(\d+))?') | ForEach-Object { [pscustomobject]@{ Max = [int]$_.Groups[1].Value; P50 = [int]$_.Groups[2].Value; P99 = [int]$_.Groups[3].Value; Count = [int]$_.Groups[4].Value; SMax = [int]$_.Groups[5].Value; SP50 = [int]$_.Groups[6].Value; SP99 = [int]$_.Groups[7].Value; SCount = [int]$_.Groups[8].Value; TMax = [int]$_.Groups[9].Value; TP50 = [int]$_.Groups[10].Value; TCount = [int]$_.Groups[11].Value } }) }
        # -Since: install-line index the round started at (the 60 s timer rehook may flush part of a round before this call).
        function Flush-CbStat([int]$Since = -1) {
            $n = (Read-CbStats).Count
            if ($Since -lt 0) { $Since = $n }
            [void][E2E]::Broadcast('TaskbarCreated')
            if (-not (Wait-Until { (Read-CbStats).Count -gt $n } 4000 100)) { return $null }
            # The 60 s timer rehook can flush part of a round first: combine every install line since this call (worst case per field).
            $new = @((Read-CbStats)[$Since..((Read-CbStats).Count - 1)])
            $big = $new | Sort-Object Count -Descending | Select-Object -First 1
            $sbig = $new | Sort-Object SCount -Descending | Select-Object -First 1
            [pscustomobject]@{
                Max = ($new | Measure-Object -Property Max -Maximum).Maximum; P50 = $big.P50; P99 = ($new | Measure-Object -Property P99 -Maximum).Maximum; Count = ($new | Measure-Object -Property Count -Sum).Sum
                SMax = ($new | Measure-Object -Property SMax -Maximum).Maximum; SP50 = $sbig.SP50; SP99 = ($new | Measure-Object -Property SP99 -Maximum).Maximum; SCount = ($new | Measure-Object -Property SCount -Sum).Sum
                TMax = ($new | Measure-Object -Property TMax -Maximum).Maximum; TP50 = ($new | Sort-Object TCount -Descending | Select-Object -First 1).TP50; TCount = ($new | Measure-Object -Property TCount -Sum).Sum
            }
        }
        $loaded = Flush-CbStat          # whole run since the previous install (busy: this script was spinning)
        $null = Flush-CbStat; Pause 1200
        $null = Focus-Explorer $explorerWin $aName
        # Space round: fresh Space keydowns reach ProbeFocus. 100 x (Space opens the preview, Space closes it) = 200 in Explorer / Viewer,
        # then 20 Spaces typed into the address-bar Edit (Pass). The Space-path histogram is flushed afterwards.
        $spaceMark = (Read-CbStats).Count
        $spaceCycles = 0
        for ($k = 0; $k -lt 100; $k++) {
            if (-not (Focus-Explorer $explorerWin $aName)) { break }
            if ($null -eq (Press-Space)) { break }
            if (-not (Wait-ViewerVisible 2000)) { break }
            Pause 150
            $hv = Get-ViewerHwnd
            if ($hv -eq [IntPtr]::Zero -or -not (Wait-Until { (Get-ForegroundHwnd) -eq $hv } 700 5)) { Close-Preview; continue }
            if ($null -eq (Press-Space)) { Close-Preview; break }
            if (-not (Wait-ViewerHidden 1500)) { Close-Preview }
            $null = Wait-Until { (Get-ForegroundHwnd) -eq $exHwnd } 800 10
            $spaceCycles++; Pause 60
        }
        $null = Focus-Explorer $explorerWin $aName
        Chord $VK.Alt 0x44 -List; Pause 400
        for ($k = 0; $k -lt 20; $k++) { $null = Press-Space -Anywhere; Pause 50 }
        Tap $VK.Esc; Pause 300
        $spaceStat = Flush-CbStat -Since $spaceMark
        Pause 1000
        $null = Focus-Explorer $explorerWin $aName
        $quietMark = (Read-CbStats).Count
        for ($k = 0; $k -lt 20; $k++) { Tap 0x7E -List; Pause 40; Tap 0x41 -List; Pause 60 }   # F15 and 'A' with a quiet system
        $quiet = Flush-CbStat -Since $quietMark
        $quietMark = (Read-CbStats).Count
        for ($k = 0; $k -lt 20; $k++) { Tap 0x7E -List; Pause 40; Tap 0x41 -List; Pause 60 }
        $quiet2 = Flush-CbStat -Since $quietMark
        Note ("hook callback over the whole run (this script busy-polls, so it includes its own CPU contention): all keys max={0} p50={1} p99={2} us n={3}; Space path max={4} p50={5} p99={6} us n={7}" -f $loaded.Max, $loaded.P50, $loaded.P99, $loaded.Count, $loaded.SMax, $loaded.SP50, $loaded.SP99, $loaded.SCount)
        $quietP99 = if ($quiet -and $quiet2) { [Math]::Max($quiet.P99, $quiet2.P99) } else { $null }
        $quietMax = if ($quiet -and $quiet2) { [Math]::Max($quiet.Max, $quiet2.Max) } else { $null }
        $quietN = if ($quiet -and $quiet2) { $quiet.Count + $quiet2.Count } else { 0 }
        $script:HookCb = [ordered]@{
            space = [ordered]@{ p50 = $spaceStat.SP50; p99 = $spaceStat.SP99; max = $spaceStat.SMax; n = $spaceStat.SCount; cycles = $spaceCycles }
            otherKeys = [ordered]@{ p99 = $quietP99; max = $quietMax; n = $quietN }
            wholeRun = [ordered]@{ max = $(if ($loaded) { $loaded.Max } else { $null }); p99 = $(if ($loaded) { $loaded.P99 } else { $null }); spaceMax = $(if ($loaded) { $loaded.SMax } else { $null }); spaceP99 = $(if ($loaded) { $loaded.SP99 } else { $null }) }
        }
        Check 'hook callback: Space path p99 <= 50 us (quiet: Space opens/closes the preview, Space in the address-bar Edit; max is reported, it may include preemption)' ($spaceStat -and $spaceStat.SCount -ge 100 -and $spaceStat.SP99 -le 50) ("(p50={0} p99={1} max={2} us, n={3} fresh Space keydowns, {4} open/close cycles; Trigger subset p50={5} max={6} us n={7}; p99 is only a real percentile for n > 100)" -f $spaceStat.SP50, $spaceStat.SP99, $spaceStat.SMax, $spaceStat.SCount, $spaceCycles, $spaceStat.TP50, $spaceStat.TMax, $spaceStat.TCount)
        Check 'hook callback: other keys p99 <= 50 us (quiet: 2 x 20 x F15 + A, 40-60 ms apart)' ($null -ne $quietP99 -and $quietP99 -le 50) ("(p99={0} us, max={1} us, n={2}; round 1 p50/p99/max={3}/{4}/{5} n={6}, round 2 p50/p99/max={7}/{8}/{9} n={10})" -f $quietP99, $quietMax, $quietN, $quiet.P50, $quiet.P99, $quiet.Max, $quiet.Count, $quiet2.P50, $quiet2.P99, $quiet2.Max, $quiet2.Count)

        Begin-Section
        # ---- hook survival (no Explorer restart: too disruptive) and daemon health ----
        Skip 'explorer restart: hook survives taskkill explorer' 'not run (too disruptive); the TaskbarCreated rehook above is the closest proxy'
        if (-not (Daemon-Alive)) { Count-DaemonDeath }
        Check 'daemon survived the whole run' (Daemon-Alive)
        $pidNow = if (Daemon-Alive) { $script:Daemon.Id } else { 0 }

        $summary = [ordered]@{
            warmWindowMs = [ordered]@{ p50 = $p50; p95 = $p95; max = (Pct $warm.ToArray() 100); n = $warm.Count }
            warmContentMs = [ordered]@{ image = [ordered]@{ p50 = $imgC50; p95 = (Pct $imageContentMs.ToArray() 95); n = $imageContentMs.Count }; text = [ordered]@{ p50 = $txtC50; p95 = (Pct $textContentMs.ToArray() 95); n = $textContentMs.Count } }
            windowVisibleMs = [ordered]@{ image = [ordered]@{ p50 = $imgP50 }; text = [ordered]@{ p50 = $txtP50 } }
            hideMs = [ordered]@{ p50 = (Pct $hideMs.ToArray() 50) }
        }
        $script:WarmSummary = $summary
    }
}
catch {
    Write-Host "FAIL unexpected error: $_ at $($_.ScriptStackTrace)"
    Write-Host $_.ScriptStackTrace
    $script:Checks.Add([pscustomobject]@{ name = 'script'; pass = $false; skipped = $false; details = "$_" })
    $script:Failed = $true
}
finally {
    try { if (Test-ViewerVisible) { Close-Preview } } catch { }
    try {
        if ($script:OrigHkl -and [FastLookWin32]::Exists($script:ExHwnd)) {
            foreach ($t in @($script:ExHwnd, $script:OrigFocus)) { if ($t -ne [IntPtr]::Zero) { [E2E]::PostMessageW($t, 0x0050, [IntPtr]::Zero, $script:OrigHkl) | Out-Null } }
            Pause 300
        }
    } catch { }
    foreach ($entry in $script:Opened) { try { Close-Explorer $entry } catch { } }
    try { Stop-Daemon } catch { }
    Get-Process -Name 'FastLook*' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
    if ($script:Failed -and (Test-Path $logFile)) { Write-Host '--- daemon.log (tail) ---'; Get-Content $logFile -Tail 60 | Write-Host }
    if ($script:Failed -and (Test-Path (Join-Path $data 'logs\viewer.log'))) { Write-Host '--- viewer.log (tail) ---'; Get-Content (Join-Path $data 'logs\viewer.log') -Tail 40 | Write-Host }
    try {
        $keep = Join-Path $repo 'out\e2e-logs'
        New-Item -ItemType Directory -Force $keep | Out-Null
        foreach ($f in 'daemon.log', 'viewer.log') { $src = Join-Path $data "logs\$f"; if (Test-Path $src) { Copy-Item $src (Join-Path $keep $f) -Force } }
        if (Test-Path $perfLog) { Copy-Item $perfLog (Join-Path $keep 'perf.log') -Force }
        if (Test-Path $viewerPerfLog) { Copy-Item $viewerPerfLog (Join-Path $keep 'perf.log.viewer') -Force }
    } catch { }
    $leftProc = @(Get-Process -Name 'FastLook*' -ErrorAction SilentlyContinue)
    $leftWin = @($script:Opened | Where-Object { -not $_.Shared -and [FastLookWin32]::Exists($_.Hwnd) })
    $leftTabs = 0
    try { foreach ($w in $shell.Windows()) { try { if ((Local-Path $w.LocationURL).StartsWith($tmp.TrimEnd('\'), 'OrdinalIgnoreCase')) { $leftTabs++ } } catch { } } } catch { }
    Write-Host ("cleanup: FastLook processes left={0}, test Explorer windows left={1}, shell windows/tabs still showing the temp folders={2}" -f $leftProc.Count, $leftWin.Count, $leftTabs)
    if ($script:LockHeld) { Remove-Item $lockPath -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

$result = [ordered]@{
    warmWindowMs = $null; warmContentMs = $null; coldWindowMs = $null
    missed = $script:Missed; crashes = $script:Crashes; peekReacted = $script:PeekReacted
    hookCallbackUs = $script:HookCb
    elevated = $isElevated; peekRunning = $script:PeekWarned; blockedKeys = $script:Blocked
    notes = $script:Notes.ToArray()
    checks = @($script:Checks | ForEach-Object { [ordered]@{ name = $_.name; pass = $_.pass; skipped = $_.skipped; details = $_.details } })
}
if ($script:WarmSummary) { foreach ($k in $script:WarmSummary.Keys) { $result[$k] = $script:WarmSummary[$k] } }
if ($script:ColdSummary) { $result['coldWindowMs'] = $script:ColdSummary.coldWindowMs }
$outFull = if ([IO.Path]::IsPathRooted($OutJson)) { $OutJson } else { Join-Path $repo $OutJson }
New-Item -ItemType Directory -Force (Split-Path -Parent $outFull) | Out-Null
$result | ConvertTo-Json -Depth 8 | Set-Content $outFull -Encoding utf8
Write-Host "JSON: $outFull"
if ($script:Failed) { Write-Host 'SUMMARY: FAIL'; exit 1 }
Write-Host 'SUMMARY: PASS'
exit 0
