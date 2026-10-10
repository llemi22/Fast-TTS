param(
    [string]$Python = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Venv = Join-Path $Root '.venv-live'
$Requirements = Join-Path $Root 'live_speech\requirements.txt'

if (-not (Test-Path $Requirements)) {
    throw "Missing live speech requirements: $Requirements"
}

function Resolve-Python {
    param([string]$Explicit)

    if ($Explicit) {
        if (-not (Get-Command $Explicit -ErrorAction SilentlyContinue)) {
            throw "Python command not found: $Explicit"
        }
        return [pscustomobject]@{ Exe = $Explicit; Args = @() }
    }

    if (Get-Command py -ErrorAction SilentlyContinue) {
        foreach ($version in @('3.12', '3.11', '3.10')) {
            & py "-$version" -c "import sys; print(sys.executable)" *> $null
            if ($LASTEXITCODE -eq 0) {
                return [pscustomobject]@{ Exe = 'py'; Args = @("-$version") }
            }
        }
        & py -3 -c "import sys; print(sys.executable)" *> $null
        if ($LASTEXITCODE -eq 0) {
            return [pscustomobject]@{ Exe = 'py'; Args = @('-3') }
        }
    }

    if (Get-Command python -ErrorAction SilentlyContinue) {
        return [pscustomobject]@{ Exe = 'python'; Args = @() }
    }

    throw 'Python 3.9+ is required for faster-whisper. Install Python and reopen PowerShell.'
}

$PythonCmd = Resolve-Python -Explicit $Python
$Exe = [string]$PythonCmd.Exe
$PrefixArgs = @($PythonCmd.Args)

Write-Host "Creating live speech environment at $Venv ..." -ForegroundColor Cyan
& $Exe @PrefixArgs -m venv $Venv
if ($LASTEXITCODE -ne 0) { throw 'Failed to create the live speech virtual environment.' }

$VenvPython = Join-Path $Venv 'Scripts\python.exe'
if (-not (Test-Path $VenvPython)) {
    throw "Virtual environment Python was not created: $VenvPython"
}

& $VenvPython -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) { throw 'pip upgrade failed.' }

& $VenvPython -m pip install -r $Requirements
if ($LASTEXITCODE -ne 0) { throw 'Live speech dependency installation failed.' }

& $VenvPython -c "import faster_whisper, numpy, sounddevice; print('Live speech Python dependencies import successfully.')"
if ($LASTEXITCODE -ne 0) { throw 'Live speech dependency import check failed.' }

& $VenvPython (Join-Path $Root 'live_speech\live_stt_tts.py') --self-test
if ($LASTEXITCODE -ne 0) { throw 'Live speech self-test failed.' }

Write-Host ''
Write-Host 'LIVE SPEECH SETUP READY.' -ForegroundColor Green
Write-Host 'Next: run .\RUN_LIVE_SPEECH.ps1' -ForegroundColor Green
Write-Host 'The first Whisper run may download the selected CTranslate2 model.' -ForegroundColor Yellow
