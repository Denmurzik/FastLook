#requires -Version 7
<#
.SYNOPSIS
  Generates the image fixtures with WPF imaging: tests/fixtures/image/{sample.png, exif-6.jpg, multi.ico, bogus.heic}.
  -Large additionally writes out/fixtures/photo24mp.jpg (6000x4000 noise + gradient, quality 90; gitignored).
#>
[CmdletBinding()]
param([switch]$Large)

$ErrorActionPreference = 'Stop'
if (-not [Threading.Thread]::CurrentThread.GetApartmentState().Equals([Threading.ApartmentState]::STA)) {
    $argsList = @('-NoProfile', '-STA', '-File', $PSCommandPath)
    if ($Large) { $argsList += '-Large' }
    & pwsh @argsList
    exit $LASTEXITCODE
}

Add-Type -AssemblyName PresentationCore, WindowsBase
Add-Type -Language CSharp -ReferencedAssemblies System.Runtime @'
public static class NoiseFill {
    public static void Fill(byte[] pixels, int width, int height, int seed) {
        var rng = new System.Random(seed);
        var noise = new byte[width * 3];
        for (int y = 0; y < height; y++) {
            rng.NextBytes(noise);
            int row = y * width * 3;
            for (int x = 0; x < width; x++) {
                int g = (x * 255 / width + y * 255 / height) / 2;
                for (int c = 0; c < 3; c++) {
                    int v = g * (c + 1) / 3 + (noise[x * 3 + c] - 128) / 4;
                    pixels[row + x * 3 + c] = (byte)(v < 0 ? 0 : v > 255 ? 255 : v);
                }
            }
        }
    }
}
'@

$repo = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $repo 'tests\fixtures\image'
New-Item -ItemType Directory -Force $dir | Out-Null

function New-Brush([string]$hex) { $b = [Windows.Media.SolidColorBrush][Windows.Media.ColorConverter]::ConvertFromString($hex); $b.Freeze(); $b }
function New-Text([string]$text, [double]$size, $brush) {
    [Windows.Media.FormattedText]::new($text, [Globalization.CultureInfo]::InvariantCulture, [Windows.FlowDirection]::LeftToRight,
        [Windows.Media.Typeface]::new('Segoe UI'), $size, $brush, 1.0)
}
function Render([int]$w, [int]$h, [scriptblock]$draw) {
    $visual = [Windows.Media.DrawingVisual]::new()
    $dc = $visual.RenderOpen()
    & $draw $dc
    $dc.Close()
    $bmp = [Windows.Media.Imaging.RenderTargetBitmap]::new($w, $h, 96, 96, [Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($visual)
    $bmp.Freeze()
    $bmp
}
function Save-Png($bitmap, [string]$path) {
    $enc = [Windows.Media.Imaging.PngBitmapEncoder]::new()
    $enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $fs = [IO.File]::Create($path); $enc.Save($fs); $fs.Dispose()
}
function Get-PngBytes($bitmap) {
    $enc = [Windows.Media.Imaging.PngBitmapEncoder]::new()
    $enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
    $ms = [IO.MemoryStream]::new(); $enc.Save($ms); , $ms.ToArray()
}

# sample.png: 320x200 gradient with the text "FastLook".
$gradient = [Windows.Media.LinearGradientBrush]::new([Windows.Media.ColorConverter]::ConvertFromString('#1E3C72'),
    [Windows.Media.ColorConverter]::ConvertFromString('#F5A623'), [Windows.Point]::new(0, 0), [Windows.Point]::new(1, 1))
$sample = Render 320 200 {
    param($dc)
    $dc.DrawRectangle($gradient, $null, [Windows.Rect]::new(0, 0, 320, 200))
    $text = New-Text 'FastLook' 44 (New-Brush '#FFFFFF')
    $dc.DrawText($text, [Windows.Point]::new((320 - $text.Width) / 2, (200 - $text.Height) / 2))
}
Save-Png $sample (Join-Path $dir 'sample.png')

# exif-6.jpg: stored 400x300 with an arrow pointing LEFT; EXIF Orientation=6 (rotate 90 CW) makes it point UP when displayed.
$arrow = Render 400 300 {
    param($dc)
    $dc.DrawRectangle((New-Brush '#FFFFFF'), $null, [Windows.Rect]::new(0, 0, 400, 300))
    $fill = New-Brush '#D0021B'
    $geo = [Windows.Media.StreamGeometry]::new()
    $ctx = $geo.Open()
    $ctx.BeginFigure([Windows.Point]::new(40, 150), $true, $true)
    foreach ($p in @(@(170, 40), @(170, 110), @(360, 110), @(360, 190), @(170, 190), @(170, 260))) {
        $ctx.LineTo([Windows.Point]::new($p[0], $p[1]), $true, $false)
    }
    $ctx.Close()
    $dc.DrawGeometry($fill, $null, $geo)
}
$meta = [Windows.Media.Imaging.BitmapMetadata]::new('jpg')
$meta.SetQuery('/app1/ifd/{ushort=274}', [uint16]6)
$jpg = [Windows.Media.Imaging.JpegBitmapEncoder]::new()
$jpg.QualityLevel = 92
$jpg.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($arrow, $null, $meta, $null))
$fs = [IO.File]::Create((Join-Path $dir 'exif-6.jpg')); $jpg.Save($fs); $fs.Dispose()

# multi.ico: 16/32/256 frames stored as PNG; each frame has its own colour so the picked frame is visible.
$frames = foreach ($spec in @(@(16, '#4A90E2'), @(32, '#7ED321'), @(256, '#D0021B'))) {
    $n = $spec[0]
    $bmp = Render $n $n {
        param($dc)
        $dc.DrawEllipse((New-Brush $spec[1]), $null, [Windows.Point]::new($n / 2, $n / 2), $n / 2 - 1, $n / 2 - 1)
        $label = New-Text "$n" ($n * 0.4) (New-Brush '#FFFFFF')
        $dc.DrawText($label, [Windows.Point]::new(($n - $label.Width) / 2, ($n - $label.Height) / 2))
    }
    [pscustomobject]@{ Size = $n; Png = [byte[]](Get-PngBytes $bmp) }
}
$ms = [IO.MemoryStream]::new(); $bw = [IO.BinaryWriter]::new($ms)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($f in $frames) {
    $dim = if ($f.Size -ge 256) { 0 } else { $f.Size }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$f.Png.Length); $bw.Write([uint32]$offset)
    $offset += $f.Png.Length
}
foreach ($f in $frames) { $bw.Write($f.Png) }
$bw.Flush()
[IO.File]::WriteAllBytes((Join-Path $dir 'multi.ico'), $ms.ToArray())

# orient-1..8.jpg: stored 400x300 quadrants TL red, TR green, BL blue, BR yellow, one file per EXIF orientation.
# scripts/check-image-orientation.ps1 compares the Viewer's rendering with the EXIF table.
$quad = Render 400 300 {
    param($dc)
    $dc.DrawRectangle((New-Brush '#FF0000'), $null, [Windows.Rect]::new(0, 0, 200, 150))
    $dc.DrawRectangle((New-Brush '#00FF00'), $null, [Windows.Rect]::new(200, 0, 200, 150))
    $dc.DrawRectangle((New-Brush '#0000FF'), $null, [Windows.Rect]::new(0, 150, 200, 150))
    $dc.DrawRectangle((New-Brush '#FFFF00'), $null, [Windows.Rect]::new(200, 150, 200, 150))
}
foreach ($o in 1..8) {
    $m = [Windows.Media.Imaging.BitmapMetadata]::new('jpg')
    $m.SetQuery('/app1/ifd/{ushort=274}', [uint16]$o)
    $e = [Windows.Media.Imaging.JpegBitmapEncoder]::new()
    $e.QualityLevel = 95
    $e.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($quad, $null, $m, $null))
    $fs = [IO.File]::Create((Join-Path $dir "orient-$o.jpg")); $e.Save($fs); $fs.Dispose()
}

# bogus.heic: 64 random bytes (not a valid image).
$rng = [Random]::new(1234); $junk = [byte[]]::new(64); $rng.NextBytes($junk)
[IO.File]::WriteAllBytes((Join-Path $dir 'bogus.heic'), $junk)
Write-Host "Wrote fixtures to $dir"

if ($Large) {
    $outDir = Join-Path $repo 'out\fixtures'
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $w = 6000; $h = 4000
    $pixels = [byte[]]::new($w * $h * 3)
    [NoiseFill]::Fill($pixels, $w, $h, 42)
    $src = [Windows.Media.Imaging.BitmapSource]::Create($w, $h, 96, 96, [Windows.Media.PixelFormats]::Bgr24, $null, $pixels, $w * 3)
    $pixels = $null
    $enc = [Windows.Media.Imaging.JpegBitmapEncoder]::new()
    $enc.QualityLevel = 90
    $enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($src))
    $path = Join-Path $outDir 'photo24mp.jpg'
    $fs = [IO.File]::Create($path); $enc.Save($fs); $fs.Dispose()
    [GC]::Collect()
    Write-Host "Wrote $path ($([math]::Round((Get-Item $path).Length / 1MB, 1)) MB)"
}
