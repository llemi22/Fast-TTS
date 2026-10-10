param(
    [string]$WhisperModel = 'turbo',
    [ValidateSet('cuda', 'cpu')]
    [string]$Device = 'cuda',
    [string]$ComputeType = 'float16',
    [string]$Language = 'en',
    [string]$InputDevice = '',
    [int]$BlockMs = 20,
    [int]$StepMs = 240,
    [int]$MinDecodeMs = 360,
    [int]$EndpointMs = 320,
    [int]$PreRollMs = 160,
    [int]$TailWords = 1,
    [int]$MinChunkWords = 2,
    [int]$MaxChunkWords = 5,
    [int]$MaxChunkDelayMs = 380,
    [float]$MaxUncommittedSeconds = 6.0,
    [int]$WarmupMs = 700,
    [float]$SpeechThresholdDb = -43,
    [switch]$ListDevices,
    [switch]$DryRun,
    [switch]$NoLaunchVoiceBox,

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
    [switch]$NoFA
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Python = Join-Path $Root '.venv-live\Scripts\python.exe'
$LiveScript = Join-Path $Root 'live_speech\live_stt_tts.py'

if (-not (Test-Path $Python)) {
    throw 'Live speech environment not found. Run .\SETUP_LIVE_SPEECH.ps1 first.'
}
if (-not (Test-Path $LiveScript)) {
    throw "Live speech bridge not found: $LiveScript"
}

if ($ListDevices) {
    & $Python $LiveScript --list-devices
    exit $LASTEXITCODE
}

if (-not $DryRun -and -not $NoLaunchVoiceBox) {
    $running = Get-Process -Name 'fast-tts-voicebox' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $running) {
        $voiceboxArgs = @{
            Detached = $true
            MaxTokens = $MaxTokens
            Temperature = $Temperature
            TopK = $TopK
            Seed = $Seed
        }
        if ($Model) { $voiceboxArgs.Model = $Model }
        if ($Codec) { $voiceboxArgs.Codec = $Codec }
        if ($ModelDir) { $voiceboxArgs.ModelDir = $ModelDir }
        if ($VoiceDir) { $voiceboxArgs.VoiceDir = $VoiceDir }
        if ($Lang) { $voiceboxArgs.Lang = $Lang }
        if ($RefWav) { $voiceboxArgs.RefWav = $RefWav }
        if ($RefText) { $voiceboxArgs.RefText = $RefText }
        if ($RefSpk) { $voiceboxArgs.RefSpk = $RefSpk }
        if ($RefRvq) { $voiceboxArgs.RefRvq = $RefRvq }
        if ($NoWarmup) { $voiceboxArgs.NoWarmup = $true }
        if ($NoFA) { $voiceboxArgs.NoFA = $true }

        Write-Host 'Starting Fast TTS VoiceBox with the resident Qwen3-TTS model...' -ForegroundColor Cyan
        & (Join-Path $Root 'RUN_VOICEBOX.ps1') @voiceboxArgs | Out-Host
    } else {
        Write-Host "Using existing Fast TTS VoiceBox process $($running.Id)." -ForegroundColor Green
    }
}

$argsList = @(
    $LiveScript,
    '--model', $WhisperModel,
    '--device', $Device,
    '--compute-type', $ComputeType,
    '--language', $Language,
    '--block-ms', "$BlockMs",
    '--step-ms', "$StepMs",
    '--min-decode-ms', "$MinDecodeMs",
    '--endpoint-ms', "$EndpointMs",
    '--pre-roll-ms', "$PreRollMs",
    '--tail-words', "$TailWords",
    '--min-chunk-words', "$MinChunkWords",
    '--max-chunk-words', "$MaxChunkWords",
    '--max-chunk-delay-ms', "$MaxChunkDelayMs",
    '--max-uncommitted-seconds', "$MaxUncommittedSeconds",
    '--warmup-ms', "$WarmupMs",
    '--speech-threshold-db', "$SpeechThresholdDb"
)

if ($InputDevice) { $argsList += @('--input-device', $InputDevice) }
if ($DryRun) { $argsList += '--dry-run' }

& $Python @argsList
exit $LASTEXITCODE
