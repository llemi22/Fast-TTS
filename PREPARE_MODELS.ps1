param(
    [string]$ModelDir = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

$Candidates = @()
if ($ModelDir) { $Candidates += $ModelDir }
if ($env:FAST_TTS_MODEL_DIR) { $Candidates += $env:FAST_TTS_MODEL_DIR }
$Candidates += @(
    (Join-Path $env:USERPROFILE 'Qwen3-TTS\models\Qwen3-TTS-GGUF-Q8_0'),
    (Join-Path $Root 'models')
)
$Candidates = $Candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique

foreach ($dir in $Candidates) {
    $talker = Join-Path $dir 'qwen-talker-1.7b-base-Q8_0.gguf'
    $codec = Join-Path $dir 'qwen-tokenizer-12hz-Q8_0.gguf'
    if ((Test-Path $talker) -and (Test-Path $codec)) {
        $env:FAST_TTS_MODEL_DIR = $dir
        Write-Host 'Fast TTS model pair found.' -ForegroundColor Green
        Write-Host "Model directory: $dir"
        Write-Host "Talker:          $talker"
        Write-Host "Tokenizer:       $codec"
        Write-Host ''
        Write-Host 'No model download or copy is required.' -ForegroundColor Green
        Write-Host 'RUN_VOICEBOX.ps1 will auto-detect this directory.' -ForegroundColor Cyan
        exit 0
    }
}

throw "The 1.7B Q8 model pair was not found. Expected qwen-talker-1.7b-base-Q8_0.gguf and qwen-tokenizer-12hz-Q8_0.gguf. Checked: $($Candidates -join '; ')"
