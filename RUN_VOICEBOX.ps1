param(
    [string]$Model = '',
    [string]$Codec = '',
    [string]$ModelDir = '',
    [string]$VoiceDir = '',
    [string]$Lang = 'English',
    [string]$RefWav = '',
    [string]$RefText = '',
    [string]$RefSpk = '',
    [string]$RefRvq = '',
    [int]$MaxTokens = 1024,
    [float]$Temperature = -1,
    [int]$TopK = -1,
    [Int64]$Seed = -2,
    [switch]$NoWarmup,
    [switch]$NoFA,
    [switch]$Detached
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$App = Join-Path $Root 'app'
$Build = Join-Path $App 'build-fast'

$ExeCandidates = @(
    (Join-Path $Build 'Release\fast-tts-voicebox.exe'),
    (Join-Path $Build 'fast-tts-voicebox.exe')
)
$Exe = $ExeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Exe -and (Test-Path $Build)) {
    $Exe = Get-ChildItem $Build -Recurse -Filter 'fast-tts-voicebox.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Exe) {
    throw 'Fast TTS executable not found. Run .\SETUP_AND_BUILD.ps1 first.'
}

$CandidateDirs = @()
if ($ModelDir) { $CandidateDirs += $ModelDir }
if ($env:FAST_TTS_MODEL_DIR) { $CandidateDirs += $env:FAST_TTS_MODEL_DIR }
$CandidateDirs += @(
    (Join-Path $env:USERPROFILE 'Qwen3-TTS\models\Qwen3-TTS-GGUF-Q8_0'),
    (Join-Path $Root 'models'),
    (Join-Path $App 'models')
)
$CandidateDirs = $CandidateDirs | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique

if (-not $Model) {
    foreach ($dir in $CandidateDirs) {
        $exact = Join-Path $dir 'qwen-talker-1.7b-base-Q8_0.gguf'
        if (Test-Path $exact) { $Model = $exact; break }
        $found = Get-ChildItem $dir -File -Filter 'qwen-talker-1.7b-base-*.gguf' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $Model = $found.FullName; break }
    }
}

if (-not $Codec) {
    foreach ($dir in $CandidateDirs) {
        $exact = Join-Path $dir 'qwen-tokenizer-12hz-Q8_0.gguf'
        if (Test-Path $exact) { $Codec = $exact; break }
        $found = Get-ChildItem $dir -File -Filter 'qwen-tokenizer-12hz-*.gguf' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $Codec = $found.FullName; break }
    }
}

if (-not $Model -or -not (Test-Path $Model)) {
    throw "1.7B talker model not found. Expected qwen-talker-1.7b-base-Q8_0.gguf. Checked: $($CandidateDirs -join '; ')"
}
if (-not $Codec -or -not (Test-Path $Codec)) {
    throw "12Hz tokenizer model not found. Expected qwen-tokenizer-12hz-Q8_0.gguf. Checked: $($CandidateDirs -join '; ')"
}

if (-not $VoiceDir) {
    $defaultVoiceDir = Join-Path $env:USERPROFILE 'Qwen3-TTS\maintained-server-data\voices-q8'
    if (Test-Path (Join-Path $defaultVoiceDir 'migration-manifest.json')) {
        $VoiceDir = $defaultVoiceDir
    }
}

Write-Host 'Using existing local models:' -ForegroundColor Green
Write-Host "  Talker:    $Model"
Write-Host "  Tokenizer: $Codec"
if ($VoiceDir) {
    Write-Host "  Voices:    $VoiceDir"
}
Write-Host ''

$Args = @(
    '--model', $Model,
    '--codec', $Codec,
    '--lang', $Lang,
    '--max-tokens', "$MaxTokens"
)
if ($VoiceDir) { $Args += @('--voice-dir', $VoiceDir) }
if ($Temperature -ge 0) { $Args += @('--temperature', "$Temperature") }
if ($TopK -ge 0) { $Args += @('--top-k', "$TopK") }
if ($Seed -ne -2) { $Args += @('--seed', "$Seed") }
if ($RefWav) { $Args += @('--ref-wav', $RefWav) }
if ($RefText) { $Args += @('--ref-text', $RefText) }
if ($RefSpk) { $Args += @('--ref-spk', $RefSpk) }
if ($RefRvq) { $Args += @('--ref-rvq', $RefRvq) }
if ($NoWarmup) { $Args += '--no-warmup' }
if ($NoFA) { $Args += '--no-fa' }

$WorkingDir = Split-Path -Parent $Exe

if ($Detached) {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Exe
    $psi.WorkingDirectory = $WorkingDir
    $psi.UseShellExecute = $false
    foreach ($arg in $Args) {
        [void]$psi.ArgumentList.Add([string]$arg)
    }
    $process = [System.Diagnostics.Process]::Start($psi)
    if (-not $process) { throw 'Fast TTS VoiceBox failed to start.' }
    Write-Host "Fast TTS VoiceBox started (PID $($process.Id))." -ForegroundColor Green
    return
}

Push-Location $WorkingDir
try {
    & $Exe @Args
} finally {
    Pop-Location
}
