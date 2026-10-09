param(
    [Parameter(Mandatory=$true)]
    [string]$Models,
    [string]$Reference = '',
    [int]$MaxTokens = 1024,
    [int]$Threads = 8,
    [int]$FirstChunkFrames = 4,
    [int]$ChunkFrames = 8,
    [int]$ContextFrames = 12,
    [switch]$NoStreaming
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Get-Location).Path
$candidates = @(
    (Join-Path $repoRoot 'build\Release\qwen3-tts-voicebox.exe'),
    (Join-Path $repoRoot 'build\qwen3-tts-voicebox.exe')
)
$exe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) { throw 'qwen3-tts-voicebox.exe was not found. Build it first.' }

$env:QWEN3_TTS_BACKEND = 'cuda'
Remove-Item Env:QWEN3_TTS_LOW_MEM -ErrorAction SilentlyContinue

$args = @(
    '-m', $Models,
    '--max-tokens', $MaxTokens,
    '--threads', $Threads,
    '--first-chunk-frames', $FirstChunkFrames,
    '--chunk-frames', $ChunkFrames,
    '--context-frames', $ContextFrames
)
if ($Reference) { $args += @('-r', $Reference) }
if ($NoStreaming) { $args += '--no-streaming' }

& $exe @args
