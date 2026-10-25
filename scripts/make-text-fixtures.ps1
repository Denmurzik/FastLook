#requires -Version 7
<#
.SYNOPSIS
  Generates the big text fixtures into out/fixtures/ (gitignored): big-200mb.log, minified-20mb.json (one line), text-10mb.txt.
  Streams to disk; never holds a whole file in memory. Existing files of the right size are kept.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $repo 'out\fixtures'
New-Item -ItemType Directory -Force $dir | Out-Null

function Write-Streamed([string]$name, [long]$minBytes, [scriptblock]$next) {
    $path = Join-Path $dir $name
    if ((Test-Path $path) -and (Get-Item $path).Length -ge $minBytes) { Write-Host "keep $name"; return }
    $stream = [IO.File]::Create($path)
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false), 1MB)
    try {
        $i = 0
        while ($stream.Length -lt $minBytes) {
            for ($k = 0; $k -lt 20000; $k++) { $writer.Write((& $next $i)); $i++ }
            $writer.Flush()
        }
    }
    finally { $writer.Dispose() }
    Write-Host ("wrote {0} ({1:N0} bytes)" -f $name, (Get-Item $path).Length)
}

$lorem = 'lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore'

# 200 MB log, ~118 bytes per line.
Write-Streamed 'big-200mb.log' (200MB) { param($i) "2026-10-25 12:00:00.000 INFO n=$i $lorem`r`n" }

# 20 MB minified JSON on a single line.
$jsonPath = Join-Path $dir 'minified-20mb.json'
if (-not ((Test-Path $jsonPath) -and (Get-Item $jsonPath).Length -ge 20MB)) {
    $stream = [IO.File]::Create($jsonPath)
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false), 1MB)
    try {
        $writer.Write('[')
        $i = 0
        while ($stream.Length -lt 20MB) {
            for ($k = 0; $k -lt 20000; $k++) {
                if ($i -gt 0) { $writer.Write(',') }
                $writer.Write("{`"id`":$i,`"name`":`"item $i`",`"tags`":[`"a`",`"b`"],`"ok`":true}")
                $i++
            }
            $writer.Flush()
        }
        $writer.Write(']')
    }
    finally { $writer.Dispose() }
    Write-Host ("wrote minified-20mb.json ({0:N0} bytes)" -f (Get-Item $jsonPath).Length)
}
else { Write-Host 'keep minified-20mb.json' }

# 10 MB prose with CRLF.
Write-Streamed 'text-10mb.txt' (10MB) { param($i) "Line $i - $lorem.`r`n" }
