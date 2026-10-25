#requires -Version 7
<#
.SYNOPSIS
  Renders assets/FastLook.ico: a #0078D4 rounded square with a white stylised eye, PNG-compressed at 16..256 px.
.EXAMPLE
  pwsh scripts/make-icon.ps1
#>
[CmdletBinding()]
param(
    [string]$Out
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $repo 'assets\FastLook.ico' }
Add-Type -AssemblyName System.Drawing

$sizes = 16, 20, 24, 32, 40, 48, 64, 256

function New-IconPng([int]$s) {
    $bmp = [System.Drawing.Bitmap]::new($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.SmoothingMode = 'AntiAlias'
        $g.PixelOffsetMode = 'HighQuality'
        $g.Clear([System.Drawing.Color]::Transparent)

        $blue = [System.Drawing.Color]::FromArgb(255, 0x00, 0x78, 0xD4)
        $white = [System.Drawing.Brushes]::White
        $blueBrush = [System.Drawing.SolidBrush]::new($blue)

        # Rounded square.
        $r = $s * 0.22
        $d = $r * 2
        $sq = [System.Drawing.Drawing2D.GraphicsPath]::new()
        $sq.AddArc(0, 0, $d, $d, 180, 90)
        $sq.AddArc($s - $d - 1, 0, $d, $d, 270, 90)
        $sq.AddArc($s - $d - 1, $s - $d - 1, $d, $d, 0, 90)
        $sq.AddArc(0, $s - $d - 1, $d, $d, 90, 90)
        $sq.CloseFigure()
        $g.FillPath($blueBrush, $sq)

        # Eye: almond made from two arcs, centred.
        $cx = $s / 2.0; $cy = $s / 2.0
        $hw = $s * 0.34       # half width
        $hh = $s * 0.22       # half height of the lens
        $eye = [System.Drawing.Drawing2D.GraphicsPath]::new()
        $left = [System.Drawing.PointF]::new($cx - $hw, $cy)
        $right = [System.Drawing.PointF]::new($cx + $hw, $cy)
        $k = 1.33 * $hh / 0.75
        $eye.AddBezier($left, [System.Drawing.PointF]::new($cx - $hw * 0.5, $cy - $k), [System.Drawing.PointF]::new($cx + $hw * 0.5, $cy - $k), $right)
        $eye.AddBezier($right, [System.Drawing.PointF]::new($cx + $hw * 0.5, $cy + $k), [System.Drawing.PointF]::new($cx - $hw * 0.5, $cy + $k), $left)
        $eye.CloseFigure()
        $g.FillPath($white, $eye)

        # Iris and pupil highlight.
        $ir = $s * 0.145
        $g.FillEllipse($blueBrush, $cx - $ir, $cy - $ir, $ir * 2, $ir * 2)
        if ($s -ge 32) {
            $hr = $s * 0.045
            $g.FillEllipse($white, $cx - $ir * 0.1 - $hr, $cy - $ir * 0.45 - $hr, $hr * 2, $hr * 2)
        }
    }
    finally { $g.Dispose() }
    $ms = [System.IO.MemoryStream]::new()
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}

$pngs = foreach ($s in $sizes) { , (New-IconPng $s) }

New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
$fs = [System.IO.File]::Create($Out)
$w = [System.IO.BinaryWriter]::new($fs)
try {
    $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]
        $dim = if ($s -ge 256) { 0 } else { $s }
        $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([uint16]1); $w.Write([uint16]32)
        $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$offset)
        $offset += $pngs[$i].Length
    }
    foreach ($p in $pngs) { $w.Write($p) }
}
finally { $w.Dispose(); $fs.Dispose() }
Write-Host "Wrote $Out ($((Get-Item $Out).Length) bytes, sizes: $($sizes -join ', '))"
