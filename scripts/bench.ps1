#requires -Version 7
<#
.SYNOPSIS
  Open/close bench (spec 9, "1000 opens"): FastLook.Viewer.exe --bench over a folder of mixed fixtures.
.DESCRIPTION
  Builds out/bench-fixtures/ (24 MP photo, 10 MB text, png, gif, md, pdf, zip, mp3, a folder, an unknown binary) unless -Dir is
  given, runs `FastLook.Viewer.exe --bench <dir> --iterations N --out <json>` with an isolated FASTLOOK_DATA_DIR, prints the JSON
  and exits with the Viewer's exit code (1 when it timed out or wrote no valid JSON). The Viewer is always killed in `finally`.
  No input is injected, but the Viewer shows and activates its own window on every open, so %TEMP%\gui-e2e.lock is held for the run.
#>
[CmdletBinding()]
param(
    [int]$Iterations = 1000,
    [string]$Dir,
    [string]$OutJson,
    [int]$TimeoutSec = 0,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\GuiLock.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "out\bin\$Configuration\FastLook.Viewer.exe"
if (-not (Test-Path $exe)) { throw "Not built: $exe (run scripts/build.ps1 first)" }
if (-not $OutJson) { $OutJson = Join-Path $repo 'out\perf\bench.json' }
elseif (-not [IO.Path]::IsPathRooted($OutJson)) { $OutJson = Join-Path $repo $OutJson }
New-Item -ItemType Directory -Force (Split-Path -Parent $OutJson) | Out-Null
Remove-Item $OutJson -ErrorAction SilentlyContinue

if (-not $Dir) {
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'make-perf-fixtures.ps1') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'make-perf-fixtures.ps1 failed' }
    $Dir = Join-Path $repo 'out\bench-fixtures'
    New-Item -ItemType Directory -Force $Dir | Out-Null
    $fx = Join-Path $repo 'out\fixtures'
    $t = Join-Path $repo 'tests\fixtures'
    $copies = @{
        'photo24mp.jpg' = Join-Path $fx 'photo24mp.jpg'
        'text-10mb.txt' = Join-Path $fx 'text-10mb.txt'
        'small.png'     = Join-Path $t 'image\sample.png'
        'anim.gif'      = Join-Path $t 'image\anim.gif'
        'readme.md'     = Join-Path $t 'markdown\readme.md'
        'doc.pdf'       = Join-Path $t 'pdf\two-pages.pdf'
        'archive.zip'   = Join-Path $t 'archive\sample.zip'
        'tone.mp3'      = Join-Path $t 'media\tone.mp3'
    }
    foreach ($name in $copies.Keys) {
        $dst = Join-Path $Dir $name
        if (-not (Test-Path $dst)) { Copy-Item $copies[$name] $dst }
    }
    $folder = Join-Path $Dir 'folder'
    if (-not (Test-Path $folder)) {
        New-Item -ItemType Directory $folder | Out-Null
        1..5 | ForEach-Object { Set-Content (Join-Path $folder "file$_.txt") "item $_" }
    }
    $bin = Join-Path $Dir 'unknown.bin'
    if (-not (Test-Path $bin)) {
        $bytes = [byte[]]::new(65536); [Random]::new(7).NextBytes($bytes); $bytes[0] = 0
        [IO.File]::WriteAllBytes($bin, $bytes)
    }
}
else {
    $Dir = (Resolve-Path $Dir).Path
}
if ($TimeoutSec -le 0) { $TimeoutSec = 120 + 3 * $Iterations }

$data = Join-Path ([IO.Path]::GetTempPath()) ("fastlook-bench-{0}" -f [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $data | Out-Null
$saved = $env:FASTLOOK_DATA_DIR
$env:FASTLOOK_DATA_DIR = $data
$proc = $null
$exit = 1
try {
    # The Viewer shows and activates its window 1000 times: hold the desktop lock for the run.
    Enter-GuiLock 'fastlook-bench'
    Write-Host "bench: $Iterations iterations over $Dir (timeout $TimeoutSec s)"
    $proc = Start-Process -FilePath $exe -ArgumentList @('--bench', "`"$Dir`"", '--iterations', $Iterations, '--out', "`"$OutJson`"") -PassThru
    if ($proc.WaitForExit($TimeoutSec * 1000)) { $exit = $proc.ExitCode }
    else { Write-Host "bench: timed out after $TimeoutSec s"; $exit = 1 }
}
finally {
    Exit-GuiLock
    if ($proc -and -not $proc.HasExited) { & taskkill.exe /T /F /PID $proc.Id *> $null }
    if ($null -ne $saved) { $env:FASTLOOK_DATA_DIR = $saved } else { Remove-Item Env:\FASTLOOK_DATA_DIR -ErrorAction SilentlyContinue }
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $OutJson)) { Write-Host 'bench: no JSON written'; exit 1 }
$text = Get-Content $OutJson -Raw
try { $null = $text | ConvertFrom-Json } catch { Write-Host "bench: invalid JSON: $text"; exit 1 }
Write-Host $text
Write-Host "bench exit code: $exit"
exit $exit
