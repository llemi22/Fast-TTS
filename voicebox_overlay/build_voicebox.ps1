param(
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Get-Location).Path

if (-not (Test-Path (Join-Path $repoRoot 'CMakeLists.txt'))) {
    throw 'Run this from the qwen3-tts.cpp repository root.'
}

if (-not (Test-Path (Join-Path $repoRoot 'ggml\CMakeLists.txt'))) {
    throw 'GGML submodule is missing. Run: git submodule update --init --recursive'
}

$ggmlBuild = Join-Path $repoRoot 'ggml\build'
$buildDir = Join-Path $repoRoot 'build'

Write-Host 'Configuring GGML with CUDA...' -ForegroundColor Cyan
cmake -S ggml -B $ggmlBuild -DGGML_CUDA=ON -DCMAKE_BUILD_TYPE=$Config
if ($LASTEXITCODE -ne 0) { throw 'GGML configure failed.' }

Write-Host 'Building GGML...' -ForegroundColor Cyan
cmake --build $ggmlBuild --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw 'GGML build failed.' }

Write-Host 'Configuring qwen3-tts.cpp...' -ForegroundColor Cyan
cmake -S . -B $buildDir -DCMAKE_BUILD_TYPE=$Config
if ($LASTEXITCODE -ne 0) { throw 'VoiceBox configure failed.' }

Write-Host 'Building VoiceBox...' -ForegroundColor Cyan
cmake --build $buildDir --config $Config --target qwen3-tts-voicebox --parallel
if ($LASTEXITCODE -ne 0) { throw 'VoiceBox build failed.' }

$candidates = @(
    (Join-Path $buildDir "$Config\qwen3-tts-voicebox.exe"),
    (Join-Path $buildDir 'qwen3-tts-voicebox.exe')
)
$exe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($exe) {
    Write-Host "Built: $exe" -ForegroundColor Green
} else {
    Write-Host 'Build finished; locate qwen3-tts-voicebox.exe under the build directory.' -ForegroundColor Yellow
}
