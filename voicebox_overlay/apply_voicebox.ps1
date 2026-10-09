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

# Materialize the large native Windows UI source from its compressed text payload.
$packedVoicebox = Join-Path $kitRoot 'src\voicebox_win.cpp.gz.b64'
$voiceboxSource = Join-Path $kitRoot 'src\voicebox_win.cpp'
if (-not (Test-Path $packedVoicebox)) {
    throw "Missing compressed VoiceBox UI payload: $packedVoicebox"
}
try {
    $encoded = (Get-Content $packedVoicebox -Raw).Trim()
    [byte[]]$compressedBytes = [Convert]::FromBase64String($encoded)
    $input = [System.IO.MemoryStream]::new($compressedBytes)
    $gzip = [System.IO.Compression.GzipStream]::new($input, [System.IO.Compression.CompressionMode]::Decompress)
    $output = [System.IO.MemoryStream]::new()
    $gzip.CopyTo($output)
    $gzip.Dispose()
    $input.Dispose()
    [System.IO.File]::WriteAllBytes($voiceboxSource, $output.ToArray())
    $output.Dispose()
} catch {
    throw "Failed to materialize voicebox_win.cpp: $($_.Exception.Message)"
}

$files = @(
    'voicebox_win.cpp',
    'tts_transformer_streaming.cpp',
    'audio_tokenizer_decoder_streaming.cpp',
    'qwen3_tts_streaming.cpp'
)
foreach ($file in $files) {
    $from = Join-Path $kitRoot "src\$file"
    if (-not (Test-Path $from)) { throw "Missing VoiceBox source file: $from" }
    Copy-Item $from (Join-Path $srcDir $file) -Force
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
