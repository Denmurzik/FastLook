#requires -Version 7
<#
.SYNOPSIS
  Smoke run (spec 10): run-preview.ps1 for every file under tests/fixtures/ (recursively; skips .ps1, .py, .gitkeep).
.DESCRIPTION
  Prints a table `file, alive, responding, privateWS_MB` and exits 1 when any run is not alive + responding. Plain screenshots
  only: no input is injected, but the Viewer windows can take the foreground, so the desktop lock is held. Screenshots go to out/shots/smoke/.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [int]$WaitMs = 1200,
    [string]$OutJson
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\GuiLock.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$root = Join-Path $repo 'tests\fixtures'
$shots = Join-Path $repo 'out\shots\smoke'
New-Item -ItemType Directory -Force $shots | Out-Null

# Every run shows a real Viewer window (it may take the foreground): hold the desktop lock for the whole sweep.
Enter-GuiLock 'fastlook-smoke'
$files = Get-ChildItem $root -Recurse -File | Where-Object { $_.Extension -notin '.ps1', '.py' -and $_.Name -ne '.gitkeep' } | Sort-Object FullName
try {
$rows = foreach ($f in $files) {
    $rel = [IO.Path]::GetRelativePath($root, $f.FullName)
    $png = Join-Path $shots (($rel -replace '[\\/:]', '_') + '.png')
    $out = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'run-preview.ps1') -File $f.FullName -Out $png -WaitMs $WaitMs -Configuration $Configuration 2>&1
    $line = @($out | Where-Object { "$_" -match '^RESULT ' }) | Select-Object -Last 1
    $alive = $false; $responding = $false; $ws = 'n/a'
    if ($line -and "$line" -match 'alive=(\w+).*responding=(\w+) privateWS_MB=(\S+)') {
        $alive = $Matches[1] -eq 'True'; $responding = $Matches[2] -eq 'True'; $ws = $Matches[3]
    }
    [pscustomobject]@{ file = $rel; alive = $alive; responding = $responding; privateWS_MB = $ws }
}
}
finally { Exit-GuiLock }

$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
$bad = @($rows | Where-Object { -not ($_.alive -and $_.responding) })
if ($OutJson) {
    $full = if ([IO.Path]::IsPathRooted($OutJson)) { $OutJson } else { Join-Path $repo $OutJson }
    New-Item -ItemType Directory -Force (Split-Path -Parent $full) | Out-Null
    [ordered]@{ total = $rows.Count; failed = $bad.Count; rows = $rows } | ConvertTo-Json -Depth 4 | Set-Content $full -Encoding utf8
}
Write-Host ("smoke: {0} files, {1} not alive+responding" -f $rows.Count, $bad.Count)
if ($bad.Count -gt 0) { $bad | ForEach-Object { Write-Host "FAIL $($_.file)" }; exit 1 }
exit 0
