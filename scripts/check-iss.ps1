#requires -Version 7
<#
.SYNOPSIS
  Static check of installer/FastLook.iss for machines without Inno Setup: required directives and sections, Source: paths, /D defines.
.EXAMPLE
  pwsh scripts/check-iss.ps1 -Iss installer/FastLook.iss -StageDir out/stage/FastLook
#>
[CmdletBinding()]
param(
    [string]$Iss = (Join-Path (Split-Path -Parent $PSScriptRoot) 'installer\FastLook.iss'),
    [Parameter(Mandatory)][string]$StageDir,
    # Version passed to ISCC as /DAppVersion. Defaults to Version.h.
    [string]$Version
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$versionH = Get-Content -Raw (Join-Path $repo 'src\Common\Version.h')
$headerVersion = [regex]::Match($versionH, '#define\s+FASTLOOK_VERSION_STR\s+"([^"]+)"').Groups[1].Value
if (-not $headerVersion) { throw 'FASTLOOK_VERSION_STR not found in Version.h' }
if (-not $Version) { $Version = $headerVersion }

$text = Get-Content -Raw -Encoding UTF8 $Iss
$lines = $text -split "`r?`n"
$script:failed = $false
function Check([string]$Name, [bool]$Ok) {
    if ($Ok) { Write-Host "  ok   $Name" } else { Write-Host "  FAIL $Name"; $script:failed = $true }
}

# Directives: expected `Name=Value` lines inside [Setup] (value compared literally).
$expected = [ordered]@{
    AppId                           = '{{6F1C2A9E-3B4D-4E8F-A1C7-5D2E9B0F4A63}'
    AppName                         = 'FastLook'
    AppPublisher                    = 'Denmurzik'
    AppPublisherURL                 = 'https://github.com/Denmurzik/FastLook'
    PrivilegesRequired              = 'lowest'
    DefaultDirName                  = '{localappdata}\Programs\FastLook'
    DisableProgramGroupPage         = 'yes'
    ArchitecturesAllowed            = 'x64compatible'
    ArchitecturesInstallIn64BitMode = 'x64compatible'
    MinVersion                      = '10.0.22621'
    OutputBaseFilename              = 'FastLook-Setup-{#AppVersion}'
    SetupIconFile                   = '..\assets\FastLook.ico'
    UninstallDisplayIcon            = '{app}\FastLook.exe'
    Compression                     = 'lzma2/max'
    SolidCompression                = 'yes'
    WizardStyle                     = 'modern'
    CloseApplications               = 'no'
}
Write-Host "check-iss: $Iss"
$setup = @{}
$section = ''
foreach ($l in $lines) {
    if ($l -match '^\[(\w+)\]\s*$') { $section = $Matches[1]; continue }
    if ($section -eq 'Setup' -and $l -match '^(\w+)=(.*)$') { $setup[$Matches[1]] = $Matches[2].Trim() }
}
foreach ($k in $expected.Keys) { Check "[Setup] $k=$($expected[$k])" ($setup[$k] -ceq $expected[$k]) }
Check '[Setup] AppVersion={#AppVersion}' ($setup['AppVersion'] -ceq '{#AppVersion}')

foreach ($s in 'Languages', 'Tasks', 'Files', 'Icons', 'Registry', 'Run', 'Code') {
    Check "section [$s]" ($text -match "(?m)^\[$s\]\s*$")
}
Check 'language english' ($text -match '(?m)^Name: "english";')
Check 'language russian' ($text -match '(?m)^Name: "russian";')
Check 'task autostart (checked by default)' ($text -match '(?m)^Name: "autostart";' -and $text -notmatch '(?m)^Name: "autostart";.*unchecked')
Check 'autostart task texts EN + RU' ($text -match 'Start FastLook with Windows' -and $text -match 'Запускать FastLook вместе с Windows')
$runKey = 'Software\Microsoft\Windows\CurrentVersion\Run'
Check 'Registry Run value (HKCU, FastLook, autostart, no uninsdeletevalue)' (
    $text.Contains('Root: HKCU; Subkey: "' + $runKey + '"; ValueType: string; ValueName: "FastLook"; ValueData: """{app}\FastLook.exe"""; Tasks: autostart; Check: ShouldWriteAutostart') -and -not $text.Contains('uninsdeletevalue'))
Check 'Icons {userprograms}\FastLook' ($text.Contains('Name: "{userprograms}\FastLook"; Filename: "{app}\FastLook.exe"'))
Check 'Run FastLook.exe nowait postinstall, no skipifsilent' (
    $text -match '(?m)^Filename: "\{app\}\\FastLook\.exe";.*nowait postinstall' -and $text -notmatch 'skipifsilent')
Check 'PrepareToInstall stops the daemon' ($text -match 'function PrepareToInstall' -and $text.Contains("'--shutdown'"))
Check 'InitializeUninstall stops the daemon' ($text -match 'function InitializeUninstall')
Check 'usPostUninstall data prompt, default No, DelTree LOCALAPPDATA' (
    $text.Contains('usPostUninstall') -and $text.Contains('MB_DEFBUTTON2') -and $text.Contains("DelTree(ExpandConstant('{localappdata}\FastLook'), True, True, True)"))
Check 'prompt texts EN + RU' ($text.Contains('Delete FastLook settings and logs?') -and $text.Contains('Удалить настройки и журналы FastLook?'))
Check 'prompt skipped when silent' ($text.Contains('if not UninstallSilent then'))
Check 'RegDeleteValue Run\FastLook (also when silent)' (
    $text.Contains("RegDeleteValue(HKEY_CURRENT_USER, RunKey, 'FastLook')") -and $text.Contains("RunKey = '$runKey'"))
Check 'silent upgrade keeps autostart state (Check: ShouldWriteAutostart)' (
    $text.Contains('function ShouldWriteAutostart') -and $text.Contains('WizardSilent and IsUpgrade') -and $text.Contains('IsUpgrade := RegKeyExists(HKEY_CURRENT_USER, UninstallKey)') -and $text.Contains("RunValueIsOurs"))
Check 'shutdown: one retry, silent taskkill fallback, interactive Retry' (
    $text.Contains('TryShutdown(Exe) = 0) or (TryShutdown(Exe) = 0)') -and $text.Contains('Stop-Process -Force') -and $text.Contains('MB_RETRYCANCEL'))
Check '[InstallDelete] removes stale runtime files' ($text -match '(?m)^\[InstallDelete\]' -and $text.Contains('Name: "{app}\*.dll"'))
Check 'UTF-8 BOM (Russian strings)' ([IO.File]::ReadAllBytes((Resolve-Path $Iss))[0..2] -join ',' -eq '239,187,191')

# Files: every Source resolves against StageDir.
$stage = (Resolve-Path $StageDir).Path
$sources = @($lines | ForEach-Object { if ($_ -match '^Source:\s*"([^"]+)"') { $Matches[1] } })
Check "[Files] has Source entries ($($sources.Count))" ($sources.Count -gt 0)
foreach ($src in $sources) {
    $resolved = $src.Replace('{#StageDir}', $stage)
    $hit = $null -ne (Get-Item -Path $resolved -ErrorAction SilentlyContinue | Select-Object -First 1)
    Check "Source resolves: $src" $hit
}
$exeOk = @('FastLook.exe', 'FastLook.Viewer.exe', 'FastLook.PreviewHost.exe') | Where-Object { -not (Test-Path (Join-Path $stage $_)) }
Check 'StageDir has the three exes' (-not $exeOk)
Check 'StageDir has no portable.txt (installed mode keeps data in %LOCALAPPDATA%)' (-not (Test-Path (Join-Path $stage 'portable.txt')))

# Defines: every {#Name} must be defined in the file or passed with /D.
$passed = @('AppVersion', 'StageDir')
$defined = @([regex]::Matches($text, '(?m)^\s*#define\s+(\w+)') | ForEach-Object { $_.Groups[1].Value })
$used = @([regex]::Matches($text, '\{#(\w+)\}') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
foreach ($u in $used) { Check "define {#$u} is passed or defined" (($passed + $defined) -contains $u) }
foreach ($p in $passed) { Check "/D$p is guarded with #ifndef + #error" ($text -match "(?s)#ifndef $p\s+#error") }
Check "-Version $Version equals Version.h $headerVersion" ($Version -eq $headerVersion)
if ($defined -contains 'AppVersion') { Check '#define AppVersion matches Version.h' ($text -match "#define\s+AppVersion\s+""$([regex]::Escape($headerVersion))""") }

if ($script:failed) { Write-Host 'check-iss: FAIL'; exit 1 }
Write-Host 'check-iss: PASS'
