# PowerShell build script for win11audio C++ DSP engine
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Test
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptDir

Write-Host "=== win11audio DSP Engine Build Tool ===" -ForegroundColor Cyan

# Locate Python
$pythonExe = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $pythonExe) {
    Write-Error "Python executable not found in PATH."
    exit 1
}

$pyVersion = & python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"
$pyPrefix = & python -c "import sys; print(sys.prefix)"
$extSuffix = & python -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))"
$pyInclude = & python -c "import sysconfig; print(sysconfig.get_path('include'))"
$pybindInclude = & python -c "import pybind11; print(pybind11.get_include())"
$pyLibDir = Join-Path $pyPrefix "libs"
$pyLibName = "python" + ($pyVersion -replace '\.', '')

Write-Host "[1/3] Python Environment:" -ForegroundColor Green
Write-Host "      Version:      $pyVersion"
Write-Host "      Prefix:       $pyPrefix"
Write-Host "      Suffix:       $extSuffix"
Write-Host "      pybind11:     $pybindInclude"

if ($Clean) {
    Write-Host "Cleaning build artifacts..." -ForegroundColor Yellow
    Get-ChildItem -Path $scriptDir -Filter "dsp_engine*.pyd" | Remove-Item -Force
    Get-ChildItem -Path $scriptDir -Filter "*.o" | Remove-Item -Force
    if (-not $Test) { exit 0 }
}

# Locate C++ Compiler
$compilerCandidate = $null

# Check w64devkit in ~/.tools
$w64devkitGpp = Join-Path $env:USERPROFILE ".tools\w64devkit\bin\g++.exe"
if (Test-Path $w64devkitGpp) {
    $compilerCandidate = $w64devkitGpp
    $env:PATH = (Join-Path $env:USERPROFILE ".tools\w64devkit\bin") + ";" + $env:PATH
} elseif (Get-Command g++ -ErrorAction SilentlyContinue) {
    $compilerCandidate = (Get-Command g++).Source
} elseif (Get-Command clang++ -ErrorAction SilentlyContinue) {
    $compilerCandidate = (Get-Command clang++).Source
}

if (-not $compilerCandidate) {
    Write-Error "No suitable C++ compiler found (checked w64devkit, g++, clang++). Please install a C++ compiler."
    exit 1
}

$outPyd = "dsp_engine" + $extSuffix
Write-Host "[2/3] Compiling C++ Extension..." -ForegroundColor Green
Write-Host "      Compiler:     $compilerCandidate"
Write-Host "      Source:       dsp_engine.cpp"
Write-Host "      Target:       $outPyd"

$compileArgs = @(
    "-O3",
    "-Wall",
    "-shared",
    "-std=c++17",
    "-I", $pyInclude,
    "-I", $pybindInclude,
    "dsp_engine.cpp",
    "-L", $pyLibDir,
    "-l$pyLibName",
    "-o", $outPyd
)

& $compilerCandidate @compileArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "Compilation failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "      [OK] Compilation succeeded: $outPyd" -ForegroundColor Green

Write-Host "[3/3] Verifying Python Module..." -ForegroundColor Green
& python -c "import dsp_engine; p = dsp_engine.AudioDSPPipeline(48000.0); print('Module loaded successfully! Available mixing modes: [MODE_PHASE_SHIFT, MODE_PHASE_MODULATOR, MODE_FREQUENCY_SHIFT]')"

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nBuild completed successfully!" -ForegroundColor Cyan
} else {
    Write-Error "Module verification failed!"
    exit 1
}
