#requires -Version 7
<#
.SYNOPSIS
  Saves a PNG of the top-level window of a running process (PrintWindow, falling back to CopyFromScreen).
.EXAMPLE
  pwsh scripts/screenshot.ps1 -ProcessId 1234 -Out out/shots/a.png
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$ProcessId,
    [Parameter(Mandatory)][string]$Out,
    [int]$TimeoutSec = 10
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib\Win32.psm1') -Force

$hwnd = Wait-TopWindow -ProcessId $ProcessId -TimeoutSec $TimeoutSec
$method = Save-WindowPng -Hwnd $hwnd -Out $Out
Write-Host "Saved $Out via $method"
