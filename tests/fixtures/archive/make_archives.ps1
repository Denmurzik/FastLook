#requires -Version 7
<#
.SYNOPSIS
  Regenerates the archive fixtures with the Windows bsdtar (C:\Windows\System32\tar.exe, not Git's GNU tar).
  sample.*: dir/a.txt, dir/sub/b.txt, dir/sub/c.txt, 100 bytes each (3 files, 2 folders).
#>
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path $tar)) { throw "$tar not found" }

$work = Join-Path ([IO.Path]::GetTempPath()) ("fl-archive-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    $text = ('FastLook archive fixture. ' * 4).Substring(0, 99) + "`n"   # exactly 100 bytes
    foreach ($rel in 'dir/a.txt', 'dir/sub/b.txt', 'dir/sub/c.txt') {
        $p = Join-Path $work $rel
        New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
        [IO.File]::WriteAllBytes($p, [Text.Encoding]::ASCII.GetBytes($text))
    }
    $cyr = Join-Path $work 'Привет'
    New-Item -ItemType Directory -Force $cyr | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $cyr 'файл.txt'), [Text.Encoding]::ASCII.GetBytes($text))
    [IO.File]::WriteAllBytes((Join-Path $work 'single.txt'), [Text.Encoding]::ASCII.GetBytes($text))

    Push-Location $work
    try {
        foreach ($f in 'sample.zip', 'sample.tar.gz', 'sample.7z', 'cyrillic.zip', 'encrypted.zip', 'single.txt.gz') {
            Remove-Item (Join-Path $here $f) -ErrorAction SilentlyContinue
        }
        & $tar -a -cf (Join-Path $here 'sample.zip') dir;            if ($LASTEXITCODE) { throw 'sample.zip' }
        & $tar -czf (Join-Path $here 'sample.tar.gz') dir;           if ($LASTEXITCODE) { throw 'sample.tar.gz' }
        & $tar --format 7zip -cf (Join-Path $here 'sample.7z') dir;  if ($LASTEXITCODE) { throw 'sample.7z' }
        & $tar -a -cf (Join-Path $here 'encrypted.zip') --options zip:encryption=zipcrypt --passphrase secret dir
        if ($LASTEXITCODE) {
            Write-Warning 'bsdtar cannot create an encrypted zip here; encrypted.zip skipped (the test skips too)'
            Remove-Item (Join-Path $here 'encrypted.zip') -ErrorAction SilentlyContinue
        }
    } finally { Pop-Location }

    # bsdtar would store the Cyrillic names in the OEM code page of the machine; Python flags them as UTF-8, on any machine.
    python -I -c "import sys,zipfile; z=zipfile.ZipFile(sys.argv[2],'w',zipfile.ZIP_DEFLATED); z.writestr('Привет/', b''); z.write(sys.argv[1]+'/Привет/файл.txt', 'Привет/файл.txt'); z.close()" `
        $work (Join-Path $here 'cyrillic.zip')
    if ($LASTEXITCODE) { throw 'cyrillic.zip' }
    python -I -c "import gzip,shutil,sys; shutil.copyfileobj(open(sys.argv[1],'rb'), gzip.open(sys.argv[2],'wb'))" `
        (Join-Path $work 'single.txt') (Join-Path $here 'single.txt.gz')
    if ($LASTEXITCODE) { throw 'single.txt.gz' }
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
Get-ChildItem $here -File | Where-Object Extension -ne '.ps1' | Format-Table Name, Length
