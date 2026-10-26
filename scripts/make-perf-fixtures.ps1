#requires -Version 7
<#
.SYNOPSIS
  Generates the large performance fixtures into out/fixtures/ (gitignored): photo24mp.jpg (+ photo24mp-2/-3.jpg copies as 24 MP
  neighbours), text-10mb.txt (and the other big text files), video-1080p.mp4 (20 s H.264/AAC through ffmpeg; falls back to a copy
  of tests/fixtures/media/clip.mp4 when ffmpeg is missing). Existing files are kept. Streams to disk; nothing big is held in RAM.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $repo 'out\fixtures'
New-Item -ItemType Directory -Force $dir | Out-Null

$photo = Join-Path $dir 'photo24mp.jpg'
if (-not (Test-Path $photo)) {
    & pwsh -NoProfile -STA -File (Join-Path $PSScriptRoot 'make-image-fixtures.ps1') -Large
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $photo)) { throw 'make-image-fixtures.ps1 -Large failed' }
}
foreach ($n in 2, 3) {
    $copy = Join-Path $dir "photo24mp-$n.jpg"
    if (-not (Test-Path $copy)) { Copy-Item $photo $copy }
}

if (-not (Test-Path (Join-Path $dir 'text-10mb.txt'))) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'make-text-fixtures.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'make-text-fixtures.ps1 failed' }
}

$video = Join-Path $dir 'video-1080p.mp4'
if (-not (Test-Path $video)) {
    $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($ffmpeg) {
        & $ffmpeg.Source -v error -y -f lavfi -i 'testsrc2=size=1920x1080:rate=30' -f lavfi -i 'sine=frequency=440:sample_rate=48000' `
            -t 20 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -shortest $video
        if ($LASTEXITCODE -ne 0) { throw 'ffmpeg failed' }
    }
    else {
        Write-Host 'ffmpeg not found: using tests/fixtures/media/clip.mp4 as the video fixture'
        Copy-Item (Join-Path $repo 'tests\fixtures\media\clip.mp4') $video
    }
}

Get-ChildItem $dir -File | ForEach-Object { Write-Host ('{0,-24} {1,10:n1} MB' -f $_.Name, ($_.Length / 1MB)) }
