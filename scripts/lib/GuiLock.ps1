# Dot-source: . "$PSScriptRoot\lib\GuiLock.ps1"
# The shared-desktop lock: another session may run GUI e2e on this machine. Enter-GuiLock waits until %TEMP%\gui-e2e.lock is gone,
# creates it, and Exit-GuiLock deletes it (call it in `finally`). A lock older than 45 minutes is considered stale.
$script:GuiLockPath = Join-Path ([IO.Path]::GetTempPath()) 'gui-e2e.lock'
$script:GuiLockHeld = $false

function Enter-GuiLock {
    param([string]$Owner = 'perf', [int]$WaitMinutes = 30)
    $end = [DateTime]::UtcNow.AddMinutes($WaitMinutes)
    while ($true) {
        try {
            if ((Test-Path $script:GuiLockPath) -and ((Get-Item $script:GuiLockPath).LastWriteTimeUtc -lt [DateTime]::UtcNow.AddMinutes(-45))) {
                Remove-Item $script:GuiLockPath -Force -ErrorAction SilentlyContinue
            }
            $fs = [IO.File]::Open($script:GuiLockPath, 'CreateNew', 'Write', 'None')
            $b = [Text.Encoding]::UTF8.GetBytes("fastlook impl-33 $Owner $([DateTime]::UtcNow.ToString('o'))")
            $fs.Write($b, 0, $b.Length); $fs.Dispose()
            $script:GuiLockHeld = $true
            return
        }
        catch [IO.IOException] {
            if ([DateTime]::UtcNow -gt $end) { throw "desktop lock $($script:GuiLockPath) is held by someone else for $WaitMinutes min" }
            Start-Sleep -Seconds 2
        }
    }
}

function Exit-GuiLock {
    if ($script:GuiLockHeld) {
        Remove-Item $script:GuiLockPath -Force -ErrorAction SilentlyContinue
        $script:GuiLockHeld = $false
    }
}
