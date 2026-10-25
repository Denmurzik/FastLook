#requires -Version 7
<#
.SYNOPSIS
  Release packaging: stages out/bin/<Configuration> (+ app-local VC++ runtime), smoke-tests the staged Viewer, then writes the
  portable zip and the Inno Setup installer to out/dist/ with .sha256 files (`<64 lowercase hex><two spaces><file name>\n`).
.DESCRIPTION
  Staging is out/stage/FastLook. Build artefacts (*.pdb/*.lib/*.exp/..., the test exe, gtest/gmock DLLs) are
  not copied; the stage must contain no WebView2 file. The zip is the stage plus an empty portable.txt (the installer does not
  ship it). Without Inno Setup (iscc) the installer is skipped with a warning after installer/FastLook.iss is checked by
  scripts/check-iss.ps1.
  RestartAgent.exe is a Windows App SDK runtime file (Microsoft, Foundation runtimes-framework) and is kept.
.EXAMPLE
  pwsh scripts/package.ps1 -NoBuild -SkipInstaller
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    # x.y.z; defaults to FASTLOOK_VERSION_STR in src/Common/Version.h.
    [string]$Version,
    [switch]$NoBuild,
    [switch]$SkipInstaller,
    # Platform toolset override passed to build.ps1, e.g. v143.
    [string]$Toolset
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'lib\Win32.psm1') -Force

if (-not $Version) {
    $header = Get-Content -Raw (Join-Path $repo 'src\Common\Version.h')
    $m = [regex]::Match($header, '#define\s+FASTLOOK_VERSION_STR\s+"([^"]+)"')
    if (-not $m.Success) { throw 'FASTLOOK_VERSION_STR not found in src/Common/Version.h' }
    $Version = $m.Groups[1].Value
}
Write-Host "Packaging FastLook $Version ($Configuration)"

if (-not $NoBuild) {
    $buildArgs = @('-Configuration', $Configuration, '-Project', 'All')
    if ($Toolset) { $buildArgs += @('-Toolset', $Toolset) }
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed with exit code $LASTEXITCODE" }
}

$bin = Join-Path $repo "out\bin\$Configuration"
$stage = Join-Path $repo 'out\stage\FastLook'
$dist = Join-Path $repo 'out\dist'
foreach ($exe in 'FastLook.exe', 'FastLook.Viewer.exe', 'FastLook.PreviewHost.exe') {
    if (-not (Test-Path (Join-Path $bin $exe))) { throw "Not built: $(Join-Path $bin $exe) (run without -NoBuild)" }
}

# --- Stage ---------------------------------------------------------------------------------------------------------------
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, $dist | Out-Null

$excludeFiles = @('*.pdb', '*.lib', '*.exp', '*.ilk', '*.iobj', '*.ipdb', '*.binlog',
    'FastLook.Core.Tests.exe', 'gtest*.dll', 'gmock*.dll')
& robocopy $bin $stage /E /NFL /NDL /NJH /NJS /NP /XF @excludeFiles | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with exit code $LASTEXITCODE" }

# App-local VC++ runtime (plan decision 7): only what the staged binaries import.
function Get-VsInstallPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
        if ($found) { return $found }
    }
    $fallback = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\18\BuildTools'
    if (Test-Path $fallback) { return $fallback }
    throw 'Visual Studio not found (needed for the VC++ redistributable DLLs).'
}
$vs = Get-VsInstallPath
$redistRoot = Join-Path $vs 'VC\Redist\MSVC'
$crtDir = Get-ChildItem $redistRoot -Directory -ErrorAction Stop |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+' } | Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue } |
    Select-Object -First 1
if (-not $crtDir) { throw "No x64 Microsoft.VC14*.CRT folder under $redistRoot" }

$dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64\\x64' } | Sort-Object FullName -Descending | Select-Object -First 1
$crtPattern = '^(msvcp140.*|vcruntime140.*|concrt140|vccorlib140)\.dll$'
if ($dumpbin) {
    $needed = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    # Transitive over the staged files only: our binaries plus the bundled Windows App SDK DLLs.
    foreach ($f in Get-ChildItem $stage -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' }) {
        foreach ($line in (& $dumpbin.FullName /nologo /dependents $f.FullName)) {
            $t = $line.Trim()
            if ($t -match $crtPattern) { [void]$needed.Add($t) }
        }
    }
    $crtNames = @($needed)
} else {
    Write-Warning 'dumpbin not found; copying msvcp140*/vcruntime140* instead of checking imports.'
    $crtNames = @(Get-ChildItem $crtDir.FullName -Filter '*.dll' | Where-Object { $_.Name -match '^(msvcp140|vcruntime140)' } | ForEach-Object Name)
}
foreach ($name in $crtNames) {
    $src = Join-Path $crtDir.FullName $name
    if (Test-Path $src) { Copy-Item $src $stage -Force } else { Write-Warning "$name is imported but not in $($crtDir.FullName)" }
}
Write-Host ("VC++ runtime from {0}: {1}" -f $crtDir.Parent.Parent.Name, ($crtNames -join ', '))
if (-not ($crtNames | Where-Object { $_ -match '^vcruntime140' })) { throw 'No vcruntime140 DLL staged; the exes cannot start on a clean machine.' }

# Licence files. THIRD_PARTY_NOTICES.md is created by the docs task.
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $stage 'LICENSE.txt') -Force
$notices = Join-Path $repo 'THIRD_PARTY_NOTICES.md'
if (Test-Path $notices) { Copy-Item $notices (Join-Path $stage 'THIRD_PARTY_NOTICES.txt') -Force }
else { Write-Warning 'THIRD_PARTY_NOTICES.md does not exist yet; not staged.' }

$bad = @(Get-ChildItem $stage -Recurse -File | Where-Object { $_.Name -match 'WebView2|^portable\.txt$|\.(pdb|lib|exp)$' })
if ($bad) { throw "Forbidden file(s) in the stage: $($bad.FullName -join ', ')" }

$stageBytes = (Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("Staged {0} files, {1:N1} MB -> {2}" -f (Get-ChildItem $stage -Recurse -File).Count, ($stageBytes / 1MB), $stage)

# --- Smoke test of the staged Viewer (not out/bin) ---------------------------------------------------------------------
$fixture = Join-Path $repo 'tests\fixtures\shell\a.txt'
$shot = Join-Path $repo 'out\shots\staged-smoke.png'
$smokeData = Join-Path ([IO.Path]::GetTempPath()) ("fastlook-smoke-{0}" -f [guid]::NewGuid().ToString('N'))
$savedDataDir = $env:FASTLOOK_DATA_DIR
$env:FASTLOOK_DATA_DIR = $smokeData
$proc = $null
try {
    $proc = Start-Process -FilePath (Join-Path $stage 'FastLook.Viewer.exe') -ArgumentList @('--preview', $fixture) -PassThru
    try { $hwnd = Wait-TopWindow -ProcessId $proc.Id -TimeoutSec 10 }
    catch { throw "Staged smoke failed: FastLook.Viewer.exe showed no window within 10 s ($($_.Exception.Message))" }
    Start-Sleep -Milliseconds 800
    if ($proc.HasExited) { throw "Staged smoke failed: FastLook.Viewer.exe exited with code $($proc.ExitCode)" }
    $method = Save-WindowPng -Hwnd $hwnd -Out $shot
    Write-Host "Staged smoke OK ($method): $shot"
} finally {
    if ($proc -and -not $proc.HasExited) { $proc.Kill($true); [void]$proc.WaitForExit(5000) }
    $env:FASTLOOK_DATA_DIR = $savedDataDir
    if (-not $savedDataDir) { Remove-Item Env:\FASTLOOK_DATA_DIR -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $smokeData -ErrorAction SilentlyContinue
}

function Write-Sha256([string]$Path) {
    $hash = (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLowerInvariant()
    $line = "$hash  $(Split-Path -Leaf $Path)`n"
    [IO.File]::WriteAllText("$Path.sha256", $line, [Text.UTF8Encoding]::new($false))
    Write-Host "sha256 $hash  $(Split-Path -Leaf $Path)"
}

# --- Portable zip: files at the archive root, plus portable.txt ----------------------------------------------------------
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = Join-Path $dist "FastLook-$Version-portable-x64.zip"
Remove-Item $zip, "$zip.sha256" -Force -ErrorAction SilentlyContinue
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
$za = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Update)
try { [void]$za.CreateEntry('portable.txt') } finally { $za.Dispose() }
Write-Sha256 $zip
Write-Host ("Portable zip: {0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))

# --- Installer ------------------------------------------------------------------------------------------------------------
$iss = Join-Path $repo 'installer\FastLook.iss'
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'check-iss.ps1') -Iss $iss -StageDir $stage -Version $Version | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw 'check-iss.ps1 failed' }

if ($SkipInstaller) {
    Write-Host 'Installer skipped (-SkipInstaller).'
} else {
    $iscc = $null
    $onPath = Get-Command iscc -ErrorAction SilentlyContinue
    if ($onPath) { $iscc = $onPath.Source }
    else {
        foreach ($c in @((Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'), (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'))) {
            if (Test-Path $c) { $iscc = $c; break }
        }
    }
    if (-not $iscc) {
        Write-Warning 'Inno Setup (iscc) not found; installer not built (check-iss.ps1 passed). CI installs Inno Setup.'
    } else {
        & $iscc /Q "/DAppVersion=$Version" "/DStageDir=$stage" "/O$dist" $iss
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
        $setup = Join-Path $dist "FastLook-Setup-$Version.exe"
        if (-not (Test-Path $setup)) { throw "ISCC did not produce $setup" }
        Write-Sha256 $setup
        Write-Host ("Installer: {0} ({1:N1} MB)" -f $setup, ((Get-Item $setup).Length / 1MB))
    }
}
Write-Host 'Package done.'
