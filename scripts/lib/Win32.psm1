#requires -Version 7
# Win32 helpers shared by the UI-verification and measurement scripts.
# Frozen after Task 2 except for Tasks 31 and 33.

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
    [DllImport("kernel32.dll")] static extern bool QueryPerformanceCounter(out long v);
    [DllImport("kernel32.dll")] static extern bool QueryPerformanceFrequency(out long v);

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

Export-ModuleMember -Function Get-PrivateWorkingSetMB, Get-PrivateBytesMB, Wait-TopWindow, Save-WindowPng,
    Get-WindowTitle, Get-WindowRect, Get-QpcNow, Read-PerfLog
