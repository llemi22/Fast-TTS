param(
    [string]$Config = 'Release',
    [switch]$Fresh,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$App = Join-Path $Root 'app'
$Overlay = Join-Path $Root 'voicebox_overlay'
$PinnedCommit = 'b3ba14077cf1b3e11b86e5f84aa9184605c89b28'
$RepoUrl = 'https://github.com/predict-woo/qwen3-tts.cpp.git'

function Require-Command([string]$Name, [string]$Hint) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name is required. $Hint"
    }
}

Require-Command git 'Install Git for Windows, then reopen PowerShell.'
Require-Command cmake 'Install CMake or the C++ CMake tools from Visual Studio 2022.'

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command py -ErrorAction SilentlyContinue }
if (-not $python) { throw 'Python 3 is required for the one-time source integration step.' }

if ($Fresh -and (Test-Path $App)) {
    Write-Host 'Removing existing app tree...' -ForegroundColor Yellow
    Remove-Item $App -Recurse -Force
}

if (-not (Test-Path (Join-Path $App '.git'))) {
    if (Test-Path $App) {
        $items = Get-ChildItem $App -Force -ErrorAction SilentlyContinue
        if ($items) { throw "The app folder exists but is not the managed Git checkout: $App. Use -Fresh if it is disposable." }
        Remove-Item $App -Force -ErrorAction SilentlyContinue
    }
    Write-Host 'Fetching pinned qwen3-tts.cpp source...' -ForegroundColor Cyan
    git clone $RepoUrl $App
    if ($LASTEXITCODE -ne 0) { throw 'Git clone failed.' }
}

Write-Host "Checking out pinned upstream commit $PinnedCommit..." -ForegroundColor Cyan
git -C $App fetch origin $PinnedCommit --depth 1
if ($LASTEXITCODE -ne 0) { throw 'Could not fetch the pinned upstream commit.' }
git -C $App checkout --detach $PinnedCommit
if ($LASTEXITCODE -ne 0) { throw 'Could not checkout the pinned upstream commit.' }

Write-Host 'Fetching the pinned GGML submodule...' -ForegroundColor Cyan
git -C $App submodule sync --recursive
git -C $App submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'GGML submodule setup failed.' }

Write-Host 'Integrating VoiceBox streaming source...' -ForegroundColor Cyan
Push-Location $App
try {
    & (Join-Path $Overlay 'apply_voicebox.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'VoiceBox source integration failed.' }
} finally {
    Pop-Location
}

$stamp = @"
Qwen3-TTS VoiceBox integrated source
Upstream: predict-woo/qwen3-tts.cpp
Upstream commit: $PinnedCommit
VoiceBox overlay: low-TTFA incremental codec-frame streaming
Generated: $(Get-Date -Format o)
"@
Set-Content -Path (Join-Path $App 'VOICEBOX_INTEGRATED_SOURCE.txt') -Value $stamp -Encoding UTF8

if ($SkipBuild) {
    Write-Host "Integrated source is ready at: $App" -ForegroundColor Green
    exit 0
}

Write-Host 'Building CUDA VoiceBox...' -ForegroundColor Cyan
Push-Location $App
try {
    & (Join-Path $Overlay 'build_voicebox.ps1') -Config $Config
    if ($LASTEXITCODE -ne 0) { throw 'VoiceBox build failed.' }
} finally {
    Pop-Location
}

$exeCandidates = @(
    (Join-Path $App "build\$Config\qwen3-tts-voicebox.exe"),
    (Join-Path $App 'build\qwen3-tts-voicebox.exe')
)
$Exe = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($Exe) {
    $ExeDir = Split-Path -Parent $Exe
    Get-ChildItem (Join-Path $App 'ggml\build') -Recurse -Filter 'ggml*.dll' -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName $ExeDir -Force }
    Write-Host ''
    Write-Host 'READY.' -ForegroundColor Green
    Write-Host "Integrated source: $App" -ForegroundColor Green
    Write-Host "Executable:       $Exe" -ForegroundColor Green
    Write-Host 'Next: put/prepare the model GGUFs, then run RUN_VOICEBOX.ps1.' -ForegroundColor Cyan
} else {
    Write-Host "Integrated source is ready at $App, but the expected EXE was not found after the build." -ForegroundColor Yellow
}
