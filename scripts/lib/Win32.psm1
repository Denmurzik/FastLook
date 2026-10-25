#requires -Version 7
# Win32 helpers shared by the UI-verification and measurement scripts.
# Frozen after Task 2 except for Tasks 31 and 33.
# Task 31 adds Send-Key, Set-ForegroundSafe, Get-ForegroundHwnd, Find-Process-Windows.

# Add-Type is not idempotent; skip when the type exists so repeated Import-Module -Force works.
if (-not ('FastLookWin32' -as [type])) {
Add-Type -AssemblyName System.Drawing.Common
# Reference every managed runtime assembly (System.Drawing.Common forwards into several private ones).
$fastLookRefs = Get-ChildItem $PSHOME -Filter *.dll | Where-Object {
    $_.Name -like 'System*' -or $_.Name -in 'netstandard.dll', 'mscorlib.dll', 'Microsoft.Win32.Primitives.dll'
} | Where-Object {
    try { [void][Reflection.AssemblyName]::GetAssemblyName($_.FullName); $true } catch { $false }
} | ForEach-Object FullName
Add-Type -ReferencedAssemblies $fastLookRefs -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public static class FastLookWin32
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_MEMORY_COUNTERS_EX2
    {
        public uint cb;
        public uint PageFaultCount;
        public UIntPtr PeakWorkingSetSize;
        public UIntPtr WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage;
        public UIntPtr QuotaPagedPoolUsage;
        public UIntPtr QuotaPeakNonPagedPoolUsage;
        public UIntPtr QuotaNonPagedPoolUsage;
        public UIntPtr PagefileUsage;
        public UIntPtr PeakPagefileUsage;
        public UIntPtr PrivateUsage;
        public UIntPtr PrivateWorkingSetSize;
        public ulong SharedCommitUsage;
    }

    delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr hwnd, uint cmd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] static extern int GetWindowTextLengthW(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out RECT r, int size);
    [DllImport("psapi.dll", SetLastError = true)]
    static extern bool GetProcessMemoryInfo(IntPtr h, out PROCESS_MEMORY_COUNTERS_EX2 c, uint cb);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern void Sleep(uint ms);
    [DllImport("kernel32.dll")] static extern bool QueryPerformanceCounter(out long v);
    [DllImport("kernel32.dll")] static extern bool QueryPerformanceFrequency(out long v);

    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern uint MapVirtualKeyW(uint code, uint mapType);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd, StringBuilder sb, int max);
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, [In] INPUT[] inputs, int size);

    [StructLayout(LayoutKind.Sequential)]
    struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public UIntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)]
    struct INPUTUNION
    {
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public long padMouse0;
        [FieldOffset(8)] public long padMouse1;
        [FieldOffset(16)] public long padMouse2;
        [FieldOffset(24)] public long padMouse3;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct INPUT { public uint type; public INPUTUNION u; }

    static Exception Fail()
    {
        return new InvalidOperationException("Win32 call failed, error " + Marshal.GetLastWin32Error());
    }

    public static void MakeDpiAware()
    {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
        SetThreadDpiAwarenessContext(new IntPtr(-4));
    }

    public static IntPtr FindTopWindow(uint pid)
    {
        IntPtr found = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows((h, l) =>
        {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid || !IsWindowVisible(h)) return true;
            if (GetWindow(h, 4 /* GW_OWNER */) != IntPtr.Zero) return true;
            RECT r; if (!GetWindowRect(h, out r)) return true;
            long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > bestArea) { bestArea = area; found = h; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static string GetTitle(IntPtr hwnd)
    {
        int n = GetWindowTextLengthW(hwnd);
        var sb = new StringBuilder(n + 1);
        GetWindowTextW(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    public static int[] GetRect(IntPtr hwnd)
    {
        RECT r;
        if (!GetWindowRect(hwnd, out r)) throw Fail();
        return new[] { r.Left, r.Top, r.Right, r.Bottom };
    }

    static PROCESS_MEMORY_COUNTERS_EX2 Query(uint pid)
    {
        // PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        IntPtr h = OpenProcess(0x1000, false, pid);
        if (h == IntPtr.Zero) throw Fail();
        try
        {
            uint cb = (uint)Marshal.SizeOf(typeof(PROCESS_MEMORY_COUNTERS_EX2));
            PROCESS_MEMORY_COUNTERS_EX2 c;
            if (!GetProcessMemoryInfo(h, out c, cb)) throw Fail();
            return c;
        }
        finally { CloseHandle(h); }
    }

    public static long PrivateWorkingSetBytes(uint pid) { return (long)Query(pid).PrivateWorkingSetSize.ToUInt64(); }
    public static long PrivateBytes(uint pid) { return (long)Query(pid).PrivateUsage.ToUInt64(); }

    public static long QpcNow() { long v; QueryPerformanceCounter(out v); return v; }
    public static long QpcFreq() { long v; QueryPerformanceFrequency(out v); return v; }


    public static IntPtr Foreground() { return GetForegroundWindow(); }
    public static bool Exists(IntPtr hwnd) { return IsWindow(hwnd); }

    public static string GetClass(IntPtr hwnd)
    {
        var sb = new StringBuilder(256);
        GetClassNameW(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    // Every top-level window (visible or not) of the processes named `name` (without .exe): { pid, hwnd, visible, class, title }.
    public static object[][] WindowsOfProcessName(string name)
    {
        var procs = System.Diagnostics.Process.GetProcessesByName(name);
        var pids = new uint[procs.Length];
        for (int i = 0; i < procs.Length; i++) { pids[i] = (uint)procs[i].Id; procs[i].Dispose(); }
        var found = new object[256][];
        int n = 0;
        if (pids.Length == 0) return new object[0][];
        EnumWindows((h, l) =>
        {
            uint p; GetWindowThreadProcessId(h, out p);
            if (Array.IndexOf(pids, p) >= 0 && n < found.Length) found[n++] = new object[] { (int)p, h, IsWindowVisible(h), GetClass(h), GetTitle(h) };
            return true;
        }, IntPtr.Zero);
        var res = new object[n][];
        Array.Copy(found, res, n);
        return res;
    }

    public static bool IsVisible(IntPtr hwnd) { return IsWindowVisible(hwnd); }

    static void Key(ushort vk, bool up)
    {
        uint flags = up ? 0x0002u : 0u; // KEYEVENTF_KEYUP
        ushort scan = (ushort)MapVirtualKeyW(vk, 0);
        // Extended keys: arrows, Insert/Delete, Home/End, PgUp/PgDn, Right Ctrl/Alt, Win keys.
        if ((vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E || vk == 0xA3 || vk == 0xA5 || vk == 0x5B || vk == 0x5C) flags |= 0x0001u;
        var inp = new INPUT[1];
        inp[0].type = 1; // INPUT_KEYBOARD
        inp[0].u.ki.wVk = vk;
        inp[0].u.ki.wScan = scan;
        inp[0].u.ki.dwFlags = flags;
        inp[0].u.ki.dwExtraInfo = UIntPtr.Zero; // 0, never FastLook's own marker (0x464C4B31)
        if (SendInput(1, inp, Marshal.SizeOf(typeof(INPUT))) != 1) throw Fail();
    }

    public static void SendKey(ushort vk, bool down, bool up)
    {
        if (down) Key(vk, false);
        if (up) Key(vk, true);
    }

    // Alt-tap trick (a tagged unassigned key between Alt down and up keeps menu bars from activating), then SetForegroundWindow.
    public static bool ForegroundSafe(IntPtr hwnd, int timeoutMs)
    {
        if (!IsWindow(hwnd)) return false;
        if (IsIconic(hwnd)) ShowWindow(hwnd, 9 /* SW_RESTORE */);
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (sw.ElapsedMilliseconds < timeoutMs)
        {
            if (GetForegroundWindow() == hwnd) return true;
            Key(0x12, false); Key(0xE8, false); Key(0xE8, true); Key(0x12, true);
            SetForegroundWindow(hwnd);
            Sleep(60);
            if (GetForegroundWindow() == hwnd) return true;
        }
        return GetForegroundWindow() == hwnd;
    }

    static bool IsUniform(Bitmap bmp)
    {
        var data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try
        {
            int stride = Math.Abs(data.Stride);
            var buf = new byte[stride * bmp.Height];
            Marshal.Copy(data.Scan0, buf, 0, buf.Length);
            uint first = BitConverter.ToUInt32(buf, 0);
            for (int y = 0; y < bmp.Height; y++)
                for (int x = 0; x < bmp.Width; x++)
                    if (BitConverter.ToUInt32(buf, y * stride + x * 4) != first) return false;
            return true;
        }
        finally { bmp.UnlockBits(data); }
    }

    // PrintWindow into a 32-bpp bitmap; saves the PNG and returns true unless the result is one uniform colour.
    public static bool TryPrintWindowPng(IntPtr hwnd, string path)
    {
        RECT r;
        if (!GetWindowRect(hwnd, out r)) throw Fail();
        int w = r.Right - r.Left, h = r.Bottom - r.Top;
        if (w <= 0 || h <= 0) throw new InvalidOperationException("Window has an empty rectangle.");
        using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        {
            using (var g = Graphics.FromImage(bmp))
            {
                IntPtr hdc = g.GetHdc();
                try { PrintWindow(hwnd, hdc, 2 /* PW_RENDERFULLCONTENT */); }
                finally { g.ReleaseHdc(hdc); }
            }
            if (IsUniform(bmp)) return false;
            bmp.Save(path, ImageFormat.Png);
            return true;
        }
    }

    public static void BringToTop(IntPtr hwnd)
    {
        ShowWindow(hwnd, 9 /* SW_RESTORE */);
        SetForegroundWindow(hwnd);
    }

    // Copies the DWM extended frame bounds of the window from the screen.
    public static void ScreenCapturePng(IntPtr hwnd, string path)
    {
        RECT f;
        if (DwmGetWindowAttribute(hwnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, out f, Marshal.SizeOf(typeof(RECT))) != 0)
        {
            if (!GetWindowRect(hwnd, out f)) throw Fail();
        }
        int fw = f.Right - f.Left, fh = f.Bottom - f.Top;
        using (var bmp = new Bitmap(fw, fh, PixelFormat.Format32bppArgb))
        {
            using (var g = Graphics.FromImage(bmp))
                g.CopyFromScreen(f.Left, f.Top, 0, 0, new Size(fw, fh), CopyPixelOperation.SourceCopy);
            bmp.Save(path, ImageFormat.Png);
        }
    }
}
'@
}

[void][FastLookWin32]::MakeDpiAware()

function Get-PrivateWorkingSetMB {
    param([Parameter(Mandatory)][int]$ProcessId)
    [FastLookWin32]::PrivateWorkingSetBytes([uint32]$ProcessId) / 1MB
}

function Get-PrivateBytesMB {
    param([Parameter(Mandatory)][int]$ProcessId)
    [FastLookWin32]::PrivateBytes([uint32]$ProcessId) / 1MB
}

# Returns the HWND ([IntPtr]) of the process' largest visible unowned top-level window; throws after the timeout.
function Wait-TopWindow {
    param([Parameter(Mandatory)][int]$ProcessId, [int]$TimeoutSec = 10)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    do {
        $h = [FastLookWin32]::FindTopWindow([uint32]$ProcessId)
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "No visible top-level window for process $ProcessId within $TimeoutSec s"
}

# Returns 'printwindow' or 'copyfromscreen' (the method that produced the PNG).
function Save-WindowPng {
    param([Parameter(Mandatory)][IntPtr]$Hwnd, [Parameter(Mandatory)][string]$Out)
    $full = [IO.Path]::GetFullPath($Out)
    New-Item -ItemType Directory -Force (Split-Path -Parent $full) | Out-Null
    if ([FastLookWin32]::TryPrintWindowPng($Hwnd, $full)) { return 'printwindow' }
    [FastLookWin32]::BringToTop($Hwnd)
    Start-Sleep -Milliseconds 300
    [FastLookWin32]::ScreenCapturePng($Hwnd, $full)
    'copyfromscreen'
}

function Get-WindowTitle {
    param([Parameter(Mandatory)][IntPtr]$Hwnd)
    [FastLookWin32]::GetTitle($Hwnd)
}

# Window rectangle in physical pixels: Left, Top, Right, Bottom, Width, Height.
function Get-WindowRect {
    param([Parameter(Mandatory)][IntPtr]$Hwnd)
    $r = [FastLookWin32]::GetRect($Hwnd)
    [pscustomobject]@{
        Left = $r[0]; Top = $r[1]; Right = $r[2]; Bottom = $r[3]
        Width = $r[2] - $r[0]; Height = $r[3] - $r[1]
    }
}

function Get-QpcNow { [FastLookWin32]::QpcNow() }

# Parses perf lines "<name> qpc=<ticks> freq=<hz> ft=<uint64> <detail>" into objects (name, qpc, freq, ft, detail).
# Lines that do not match are skipped.
function Read-PerfLog {
    param([Parameter(Mandatory)][string]$Path)
    $re = '^(?<name>\S+)\s+qpc=(?<qpc>\d+)\s+freq=(?<freq>\d+)\s+ft=(?<ft>\d+)(?:\s+(?<detail>.*?))?\s*$'
    foreach ($line in [IO.File]::ReadLines($Path)) {
        $m = [regex]::Match($line, $re)
        if (-not $m.Success) { continue }
        [pscustomobject]@{
            name   = $m.Groups['name'].Value
            qpc    = [int64]$m.Groups['qpc'].Value
            freq   = [int64]$m.Groups['freq'].Value
            ft     = [uint64]$m.Groups['ft'].Value
            detail = $m.Groups['detail'].Value
        }
    }
}

function Get-ForegroundHwnd { [FastLookWin32]::Foreground() }

# Windows of all processes named $Name (no .exe), visible or not: Pid, Hwnd, Visible, Class, Title.
function Find-Process-Windows {
    param([Parameter(Mandatory)][string]$Name)
    foreach ($w in [FastLookWin32]::WindowsOfProcessName($Name)) {
        [pscustomobject]@{ Pid = [int]$w[0]; Hwnd = [IntPtr]$w[1]; Visible = [bool]$w[2]; Class = [string]$w[3]; Title = [string]$w[4] }
    }
}

# SendInput of one virtual key with dwExtraInfo = 0 (so FastLook's own-injection marker never matches). Default: press and release.
function Send-Key {
    param([Parameter(Mandatory)][int]$Vk, [switch]$Down, [switch]$Up)
    $d = $Down.IsPresent; $u = $Up.IsPresent
    if (-not $d -and -not $u) { $d = $true; $u = $true }
    [FastLookWin32]::SendKey([uint16]$Vk, $d, $u)
}

# Alt-tap trick + SetForegroundWindow; $true when $Hwnd is the foreground window afterwards.
function Set-ForegroundSafe {
    param([Parameter(Mandatory)][IntPtr]$Hwnd, [int]$TimeoutMs = 1500)
    [FastLookWin32]::ForegroundSafe($Hwnd, $TimeoutMs)
}

Export-ModuleMember -Function Get-PrivateWorkingSetMB, Get-PrivateBytesMB, Wait-TopWindow, Save-WindowPng,
    Get-WindowTitle, Get-WindowRect, Get-QpcNow, Read-PerfLog, Get-ForegroundHwnd, Find-Process-Windows, Send-Key,
    Set-ForegroundSafe
