param([string]$Exe = (Join-Path $PSScriptRoot '..\PDFReader.exe'))
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path
$temporary = Join-Path ([System.IO.Path]::GetTempPath()) ('MinimalPDFReaderTest-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $temporary | Out-Null
Write-Host "Diagnostics directory: $temporary"

function Run-PdfCase([string]$File,[bool]$ExpectSuccess) {
    $arguments = '--diagnostic "' + $File + '"'
    $process = Start-Process -FilePath $Exe -ArgumentList $arguments -WorkingDirectory $temporary -PassThru
    if (-not $process.WaitForExit(90000)) {
        $process.Kill()
        throw 'Diagnostic timed out after 90 seconds.'
    }
    $process.Refresh()
    $log = Get-Content (Join-Path $temporary 'PDFReader-diagnostic.txt') -Raw
    Write-Host $log
    if ($ExpectSuccess -and ($process.ExitCode -ne 0 -or $log -notmatch 'render: PASS')) {
        throw 'Expected successful native Windows PDF load and render.'
    }
    if (-not $ExpectSuccess -and ($process.ExitCode -eq 0 -or $log -notmatch 'load: FAIL')) {
        throw 'Expected corrupt PDF rejection.'
    }
}

$unicode = Join-Path $temporary '中文 PDF 測試.pdf'
Copy-Item (Join-Path $PSScriptRoot 'mixed-pages.pdf') $unicode
Run-PdfCase $unicode $true
Run-PdfCase (Join-Path $PSScriptRoot 'broken.pdf') $false
Write-Host 'PASS: native Windows PDF decoding/rendering, Unicode path, corrupt-file handling.'
Write-Host 'Touchscreen / touchpad gestures and visual layout still require manual testing.'
