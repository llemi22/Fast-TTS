param(
    [string]$Config = 'Release',
    [switch]$Fresh,
    [switch]$SkipBuild,
    [switch]$CpuOnly,
    [string]$CudaArch = 'native'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$App = Join-Path $Root 'app'
$Overlay = Join-Path $Root 'voicebox_17'
$PinnedCommit = '51512f129a7419567f4b8abfb06801451789b8f1'
$RepoUrl = 'https://github.com/ServeurpersoCom/qwentts.cpp.git'

function Require-Command([string]$Name, [string]$Hint) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "$Name is required. $Hint"
    }
}

function Get-VSGenerator {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }

    $version = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
    if (-not $version) { return $null }
    $major = [int]($version.Split('.')[0])
    if ($major -ge 18) { return 'Visual Studio 18 2026' }
    if ($major -eq 17) { return 'Visual Studio 17 2022' }
    return $null
}

Require-Command git 'Install Git for Windows and reopen PowerShell.'
Require-Command cmake 'Install CMake and the Visual Studio C++ Desktop workload.'

if ($Fresh -and (Test-Path $App)) {
    Write-Host 'Removing existing integrated backend...' -ForegroundColor Yellow
    Remove-Item $App -Recurse -Force
}

if (-not (Test-Path (Join-Path $App '.git'))) {
    if (Test-Path $App) {
        $items = Get-ChildItem $App -Force -ErrorAction SilentlyContinue
        if ($items) {
            throw "The app folder exists but is not the managed Git checkout: $App. Re-run with -Fresh if it is disposable."
        }
        Remove-Item $App -Force -ErrorAction SilentlyContinue
    }
    Write-Host 'Fetching qwentts.cpp 1.7B backend...' -ForegroundColor Cyan
    git clone $RepoUrl $App
    if ($LASTEXITCODE -ne 0) { throw 'Backend clone failed.' }
}

Write-Host "Checking out pinned qwentts.cpp commit $PinnedCommit..." -ForegroundColor Cyan
git -C $App fetch origin $PinnedCommit --depth 1
if ($LASTEXITCODE -ne 0) { throw 'Could not fetch the pinned backend commit.' }
git -C $App checkout --detach $PinnedCommit
if ($LASTEXITCODE -ne 0) { throw 'Could not checkout the pinned backend commit.' }

Write-Host 'Fetching pinned GGML submodule...' -ForegroundColor Cyan
git -C $App submodule sync --recursive
git -C $App submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'GGML submodule setup failed.' }

$VoiceboxSource = Join-Path $Overlay 'fast_tts_voicebox.cpp'
if (-not (Test-Path $VoiceboxSource)) { throw "Missing Fast TTS source: $VoiceboxSource" }
Copy-Item $VoiceboxSource (Join-Path $App 'tools\fast-tts-voicebox.cpp') -Force

$CMake = Join-Path $App 'CMakeLists.txt'
$Marker = '# FAST_TTS_17_VOICEBOX_TARGET'
$CMakeText = Get-Content $CMake -Raw
if ($CMakeText -notmatch [regex]::Escape($Marker)) {
    $Block = @'

# FAST_TTS_17_VOICEBOX_TARGET
if(WIN32)
    add_executable(fast-tts-voicebox WIN32 tools/fast-tts-voicebox.cpp)
    target_link_libraries(fast-tts-voicebox PRIVATE qwen-core winmm shell32)
    link_ggml_backends(fast-tts-voicebox)
endif()
'@
    Add-Content -Path $CMake -Value $Block -Encoding UTF8
}

$Stamp = @"
Fast TTS 1.7B integrated source
Backend: ServeurpersoCom/qwentts.cpp
Backend commit: $PinnedCommit
Model target: qwen-talker-1.7b-base-Q8_0.gguf
Codec target: qwen-tokenizer-12hz-Q8_0.gguf
Streaming: qwentts.cpp native stateful frame streaming
Generated: $(Get-Date -Format o)
"@
Set-Content -Path (Join-Path $App 'FAST_TTS_INTEGRATED_SOURCE.txt') -Value $Stamp -Encoding UTF8

if ($SkipBuild) {
    Write-Host "Integrated 1.7B source is ready at: $App" -ForegroundColor Green
    exit 0
}

$Build = Join-Path $App 'build-fast'
if (Test-Path $Build) { Remove-Item $Build -Recurse -Force }

$Gen = Get-VSGenerator
$Configure = @('-S', $App, '-B', $Build)
if ($Gen) { $Configure += @('-G', $Gen, '-A', 'x64') }

if ($CpuOnly) {
    $Configure += '-DGGML_CUDA=OFF'
    Write-Host 'Configuring CPU validation build...' -ForegroundColor Cyan
} else {
    if (-not (Get-Command nvcc -ErrorAction SilentlyContinue)) {
        Write-Host 'nvcc was not found on PATH. CMake may still find CUDA through Visual Studio, but CUDA Toolkit 12.8+ is recommended for RTX 50-series.' -ForegroundColor Yellow
    }
    $Configure += '-DGGML_CUDA=ON'
    if ($CudaArch) { $Configure += "-DCMAKE_CUDA_ARCHITECTURES=$CudaArch" }
    Write-Host 'Configuring CUDA build for Fast TTS...' -ForegroundColor Cyan
}

cmake @Configure
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

cmake --build $Build --config $Config --target fast-tts-voicebox --parallel
if ($LASTEXITCODE -ne 0) { throw 'Fast TTS build failed.' }

$ExeCandidates = @(
    (Join-Path $Build "$Config\fast-tts-voicebox.exe"),
    (Join-Path $Build 'fast-tts-voicebox.exe')
)
$Exe = $ExeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Exe) {
    $Exe = Get-ChildItem $Build -Recurse -Filter 'fast-tts-voicebox.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Exe) { throw 'Build completed but fast-tts-voicebox.exe was not found.' }

Write-Host ''
Write-Host 'FAST TTS 1.7B BUILD READY.' -ForegroundColor Green
Write-Host "Executable: $Exe" -ForegroundColor Green
if (-not $CpuOnly) {
    Write-Host 'Next: run .\RUN_VOICEBOX.ps1. It auto-detects your existing Qwen3-TTS model folder.' -ForegroundColor Cyan
}
