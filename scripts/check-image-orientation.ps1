#requires -Version 7
<#
.SYNOPSIS
  Verifies EXIF orientation rendering. For each tests/fixtures/image/orient-N.jpg, runs scripts/run-preview.ps1 and checks the
  colour of the four quadrants of the shown image against the EXIF specification (TL/TR/BL/BR as R,G,B,Y when stored upright).
  Run via the test lock like any GUI run: <TL> pwsh -NoProfile -File scripts/check-image-orientation.ps1
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$repo = Split-Path -Parent $PSScriptRoot
# Expected displayed [TL, TR, BL, BR] per orientation, from the EXIF spec (R,G,B,Y = stored TL,TR,BL,BR).
$expected = @{
    1 = 'R G B Y'; 2 = 'G R Y B'; 3 = 'Y B G R'; 4 = 'B Y R G'
    5 = 'R B G Y'; 6 = 'B R Y G'; 7 = 'Y G B R'; 8 = 'G Y R B'
}
$palette = @{ R = @(255, 0, 0); G = @(0, 255, 0); B = @(0, 0, 255); Y = @(255, 255, 0) }
function Get-Letter([System.Drawing.Color]$c) {
    $best = $null; $bestD = [int]::MaxValue
    foreach ($k in $palette.Keys) {
        $p = $palette[$k]; $d = [math]::Pow($c.R - $p[0], 2) + [math]::Pow($c.G - $p[1], 2) + [math]::Pow($c.B - $p[2], 2)
        if ($d -lt $bestD) { $bestD = $d; $best = $k }
    }
    if ($bestD -gt 12000) { return '?' }
    $best
}
$failed = 0
foreach ($o in 1..8) {
    $png = Join-Path $repo "out\shots\orient-$o.png"
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'run-preview.ps1') -File (Join-Path $repo "tests\fixtures\image\orient-$o.jpg") -Out $png -WaitMs 1200 | Out-Null
    $bmp = [System.Drawing.Bitmap]::new($png)
    try {
        # Bounding box of saturated pixels = the image.
        $minX = $bmp.Width; $minY = $bmp.Height; $maxX = 0; $maxY = 0
        for ($y = 0; $y -lt $bmp.Height; $y += 2) { for ($x = 0; $x -lt $bmp.Width; $x += 2) {
            if ((Get-Letter $bmp.GetPixel($x, $y)) -ne '?') { $minX = [math]::Min($minX, $x); $maxX = [math]::Max($maxX, $x); $minY = [math]::Min($minY, $y); $maxY = [math]::Max($maxY, $y) }
        } }
        $w = $maxX - $minX; $h = $maxY - $minY
        $got = foreach ($q in @(@(0.25, 0.25), @(0.75, 0.25), @(0.25, 0.75), @(0.75, 0.75))) {
            Get-Letter $bmp.GetPixel([int]($minX + $w * $q[0]), [int]($minY + $h * $q[1]))
        }
        $text = $got -join ' '
        $ok = $text -eq $expected[$o]
        if (-not $ok) { $failed++ }
        '{0} orientation {1}: got [{2}] expected [{3}] ({4}x{5})' -f ($ok ? 'PASS' : 'FAIL'), $o, $text, $expected[$o], $w, $h
    } finally { $bmp.Dispose() }
}
exit ($failed -gt 0 ? 1 : 0)
