$ErrorActionPreference = 'Stop'

$kitRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Get-Location).Path
$cmake = Join-Path $repoRoot 'CMakeLists.txt'
$srcDir = Join-Path $repoRoot 'src'

if (-not (Test-Path $cmake)) {
    throw "Run this script from the root of predict-woo/qwen3-tts.cpp (CMakeLists.txt not found)."
}
if (-not (Test-Path $srcDir)) {
    throw "src directory not found. This does not look like the qwen3-tts.cpp repo root."
}

$files = @(
    'voicebox_win.cpp',
    'tts_transformer_streaming.cpp',
    'audio_tokenizer_decoder_streaming.cpp',
    'qwen3_tts_streaming.cpp'
)
foreach ($file in $files) {
    Copy-Item (Join-Path $kitRoot "src\$file") (Join-Path $srcDir $file) -Force
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command py -ErrorAction SilentlyContinue
}
if (-not $python) {
    throw "Python is required only to apply the source patch. Python is not required by the finished VoiceBox runtime."
}

if ($python.Name -eq 'py.exe' -or $python.Name -eq 'py') {
    & $python.Source -3 (Join-Path $kitRoot 'patch_streaming.py') $repoRoot
} else {
    & $python.Source (Join-Path $kitRoot 'patch_streaming.py') $repoRoot
}
if ($LASTEXITCODE -ne 0) {
    throw "Streaming patch failed. See the error above; no blind fallback edits were attempted."
}

Write-Host 'VoiceBox + low-TTFA streaming engine installed.' -ForegroundColor Green
Write-Host 'Upstream headers/CMake were backed up with .voicebox_streaming.bak suffixes.' -ForegroundColor DarkGray
Write-Host 'Next: run .\build_voicebox.ps1' -ForegroundColor Cyan
