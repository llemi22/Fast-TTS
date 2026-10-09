param(
    [string]$Models = '',
    [string]$Reference = '',
    [int]$MaxTokens = 1024,
    [int]$Threads = 8,
    [int]$FirstChunkFrames = 4,
    [int]$ChunkFrames = 8,
    [int]$ContextFrames = 12,
    [switch]$NoStreaming
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$App = Join-Path $Root 'app'
$Runner = Join-Path $Root 'voicebox_overlay\run_voicebox.ps1'
if (-not (Test-Path $Runner)) { throw 'VoiceBox runner is missing from the package.' }
if (-not (Test-Path (Join-Path $App 'CMakeLists.txt'))) { throw 'Run SETUP_AND_BUILD.ps1 first.' }
if (-not $Models) {
    $candidate = Join-Path $App 'models'
    if (Test-Path $candidate) { $Models = $candidate }
    else { $Models = Join-Path $Root 'models' }
}
Push-Location $App
try {
    $args = @{
        Models = $Models
        MaxTokens = $MaxTokens
        Threads = $Threads
        FirstChunkFrames = $FirstChunkFrames
        ChunkFrames = $ChunkFrames
        ContextFrames = $ContextFrames
    }
    if ($Reference) { $args.Reference = $Reference }
    if ($NoStreaming) { $args.NoStreaming = $true }
    & $Runner @args
} finally {
    Pop-Location
}
