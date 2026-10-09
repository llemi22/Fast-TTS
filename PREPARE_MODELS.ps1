param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$App = Join-Path $Root 'app'
if (-not (Test-Path (Join-Path $App 'scripts\setup_pipeline_models.py'))) {
    throw 'Run SETUP_AND_BUILD.ps1 (or SETUP_AND_BUILD.ps1 -SkipBuild) first.'
}

$uv = Get-Command uv -ErrorAction SilentlyContinue
if (-not $uv) {
    throw 'uv is recommended for model setup. Install it with: powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"'
}

$venv = Join-Path $App '.venv-models'
if (-not (Test-Path $venv)) {
    Write-Host 'Creating isolated Python 3.12 environment for model conversion...' -ForegroundColor Cyan
    uv venv --python 3.12 $venv
    if ($LASTEXITCODE -ne 0) { throw 'Failed to create model setup environment.' }
}
$py = Join-Path $venv 'Scripts\python.exe'
Write-Host 'Installing model conversion dependencies...' -ForegroundColor Cyan
uv pip install --python $py --upgrade pip
uv pip install --python $py huggingface_hub gguf torch safetensors numpy tqdm
if ($LASTEXITCODE -ne 0) { throw 'Model conversion dependency installation failed.' }

Push-Location $App
try {
    $args = @('scripts/setup_pipeline_models.py', '--coreml', 'off')
    if ($Force) { $args += '--force' }
    & $py @args
    if ($LASTEXITCODE -ne 0) { throw 'Model setup failed.' }
} finally {
    Pop-Location
}

Write-Host "Models are prepared under $App\models" -ForegroundColor Green
