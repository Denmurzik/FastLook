# Lists large files
param([string]$Path = '.', [int]$MinMB = 10)

function Get-BigFiles {
    param($Dir, $Limit)
    Get-ChildItem -Path $Dir -Recurse -File |
        Where-Object { $_.Length -gt ($Limit * 1MB) } |
        Sort-Object Length -Descending
}

foreach ($f in Get-BigFiles -Dir $Path -Limit $MinMB) {
    if ($f.Length -gt 100MB) {
        Write-Host "huge: $($f.FullName)"
    } else {
        Write-Output "big: $($f.Name) ($([math]::Round($f.Length / 1MB, 1)) MB)"
    }
}
