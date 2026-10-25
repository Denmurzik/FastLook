#requires -Version 7
<#
.SYNOPSIS
  Restore + build FastLook from the command line (Visual Studio Build Tools only, no IDE).
.EXAMPLE
  pwsh scripts/build.ps1 -Configuration Release
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\Current\Bin\MSBuild.exe' | Select-Object -First 1
        if ($found) { return $found }
    }
    $fallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
    if (Test-Path $fallback) { return $fallback }
    throw 'MSBuild.exe not found. Install Visual Studio Build Tools with the "Desktop development with C++" workload.'
}

$msbuild = Find-MSBuild
Write-Host "MSBuild: $msbuild"

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$sw = [System.Diagnostics.Stopwatch]::StartNew()

& $msbuild (Join-Path $repo 'FastLook.sln') `
    /restore /t:$target `
    /p:Configuration=$Configuration /p:Platform=x64 `
    /m /nologo /v:minimal /clp:Summary `
    "/bl:$(Join-Path $repo "out\msbuild-$Configuration.binlog")"
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE" }

$sw.Stop()
$exe = Join-Path $repo "out\bin\$Configuration\FastLook.exe"
if (-not (Test-Path $exe)) { throw "Build reported success but $exe does not exist" }

$size = (Get-ChildItem (Split-Path $exe) -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ('Build time: {0:n1}s; output folder: {1:n1} MB' -f $sw.Elapsed.TotalSeconds, ($size / 1MB))
Write-Host "Output: $exe"
