#requires -Version 7
<#
.SYNOPSIS
  Install vcpkg dependencies, then restore + build FastLook from the command line (Visual Studio Build Tools only, no IDE).
.EXAMPLE
  pwsh scripts/build.ps1 -Configuration Release -Project All
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateSet('All', 'Core', 'Tests', 'Daemon', 'Viewer', 'PreviewHost')]
    [string]$Project = 'All',
    # Platform toolset override, e.g. v143. Default comes from Directory.Build.props.
    [string]$Toolset,
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Get-VsInstallPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath |
            Select-Object -First 1
        if ($found) { return $found }
    }
    $fallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\18\BuildTools'
    if (Test-Path $fallback) { return $fallback }
    throw 'Visual Studio not found. Install Visual Studio Build Tools with the "Desktop development with C++" workload.'
}

function Find-MSBuild([string]$vs) {
    $msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
    if (-not (Test-Path $msbuild)) { throw "MSBuild.exe not found under $vs" }
    $msbuild
}

# Same order as Directory.Build.props: VCPKG_ROOT, VCPKG_INSTALLATION_ROOT, the VS-bundled copy.
function Find-Vcpkg([string]$vs) {
    foreach ($root in @($env:VCPKG_ROOT, $env:VCPKG_INSTALLATION_ROOT, (Join-Path $vs 'VC\vcpkg'))) {
        if ($root -and (Test-Path (Join-Path $root 'vcpkg.exe'))) { return (Join-Path $root 'vcpkg.exe') }
    }
    throw 'vcpkg.exe not found (set VCPKG_ROOT or install the "vcpkg package manager" VS component).'
}

$vs = Get-VsInstallPath
$msbuild = Find-MSBuild $vs
$vcpkg = Find-Vcpkg $vs
Write-Host "MSBuild: $msbuild"
Write-Host "vcpkg:   $vcpkg"

New-Item -ItemType Directory -Force (Join-Path $repo 'out') | Out-Null

# Keep everything vcpkg writes inside out/ (the bundled copy lives under Program Files).
$vcpkgWork = Join-Path $repo 'out\vcpkg_work'
if (-not $env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS = Join-Path $vcpkgWork 'downloads' }
& $vcpkg install --triplet x64-windows-static-md `
    "--x-manifest-root=$repo" "--x-install-root=$(Join-Path $repo 'out\vcpkg_installed')" `
    "--x-buildtrees-root=$(Join-Path $vcpkgWork 'buildtrees')" "--x-packages-root=$(Join-Path $vcpkgWork 'packages')" `
    --clean-after-build
if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed with exit code $LASTEXITCODE" }

$projectFiles = @{
    Core        = 'src\Core\FastLook.Core.vcxproj'
    Tests       = 'tests\Core.Tests\FastLook.Core.Tests.vcxproj'
    Daemon      = 'src\Daemon\FastLook.Daemon.vcxproj'
    Viewer      = 'src\Viewer\FastLook.Viewer.vcxproj'
    PreviewHost = 'src\PreviewHost\FastLook.PreviewHost.vcxproj'
}
$buildTarget = if ($Project -eq 'All') { 'FastLook.sln' } else { $projectFiles[$Project] }

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$msbuildArgs = @(
    (Join-Path $repo $buildTarget),
    '/restore', "/t:$target",
    "/p:Configuration=$Configuration", '/p:Platform=x64',
    '/m:2', '/p:CL_MPCount=4', '/nologo', '/v:minimal', '/clp:Summary',
    "/bl:$(Join-Path $repo "out\msbuild-$Configuration.binlog")"
)
if ($Toolset) { $msbuildArgs += "/p:FastLookToolset=$Toolset" }

$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $msbuild @msbuildArgs
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed with exit code $LASTEXITCODE" }
$sw.Stop()

$outDir = Join-Path $repo "out\bin\$Configuration"
if ($Project -eq 'All') {
    foreach ($exe in 'FastLook.exe', 'FastLook.Viewer.exe', 'FastLook.PreviewHost.exe') {
        if (-not (Test-Path (Join-Path $outDir $exe))) { throw "Build reported success but $exe is missing from $outDir" }
    }
}

$size = (Get-ChildItem $outDir -Recurse -File | Measure-Object Length -Sum).Sum
$sizeNoPdb = (Get-ChildItem $outDir -Recurse -File -Exclude *.pdb | Measure-Object Length -Sum).Sum
Write-Host 'Build succeeded'
Write-Host ('Build time: {0:n1}s; output folder: {1:n1} MB ({2:n1} MB without .pdb)' -f $sw.Elapsed.TotalSeconds, ($size / 1MB), ($sizeNoPdb / 1MB))
Write-Host "Output: $outDir"
