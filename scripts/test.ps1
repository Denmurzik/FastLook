#requires -Version 7
<#
.SYNOPSIS
  Build (unless -NoBuild) and run FastLook.Core.Tests. Exit code is the test executable's.
.EXAMPLE
  pwsh scripts/test.ps1 -Filter 'Utf.*'
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$Filter = '*',
    [switch]$NoBuild,
    # Sets FASTLOOK_E2E=1 so end-to-end tests (real Explorer, real windows) are not skipped.
    [switch]$E2E
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration -Project Tests
}

$exe = Join-Path $repo "out\bin\$Configuration\FastLook.Core.Tests.exe"
if (-not (Test-Path $exe)) { throw "$exe not found (run without -NoBuild)" }

$xml = Join-Path $repo 'out\test-results.xml'
if ($E2E) { $env:FASTLOOK_E2E = '1' }
try {
    & $exe "--gtest_filter=$Filter" "--gtest_output=xml:$xml"
    $code = $LASTEXITCODE
}
finally {
    if ($E2E) { Remove-Item Env:\FASTLOOK_E2E -ErrorAction SilentlyContinue }
}
exit $code
