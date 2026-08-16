param(
    [Parameter(Mandatory=$false)]
    [string]$Port = "COM20",

    [Parameter(Mandatory=$false)]
    [ValidateSet("node2node", "android2node")]
    [string]$App = "node2node",

    [Parameter(Mandatory=$false)]
    [int]$Baud = 460800
)

# Setup Environment
$env:IDF_TOOLS_PATH = "C:\Users\stefa\OneDrive\Documents\ESP\.esptools"
$env:IDF_PYTHON_ENV_PATH = "C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env"
$env:PATH = "C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env\Scripts;" + $env:PATH
& "C:\Users\stefa\OneDrive\Documents\ESP\v5.2\esp-idf\export.ps1" | Out-Null

$appBinName = if ($App -eq "node2node") { "esp32c6_ble_audio_broadcast.bin" } else { "esp32c6_ble_audio_receiver.bin" }
$buildDir = "apps\$App\build"

$bootloader = "$buildDir\bootloader\bootloader.bin"
$partition = "$buildDir\partition_table\partition-table.bin"
$appBin = "$buildDir\$appBinName"

if (-not (Test-Path $appBin)) {
    Write-Error "Binary not found at $appBin. Please build the project first using 'idf.py -C apps\$App build'."
    exit 1
}

Write-Host "Flashing $App firmware to $Port at $Baud baud..." -ForegroundColor Cyan

python -m esptool --chip esp32c6 -p $Port -b $Baud --before default_reset --after hard_reset write_flash `
    --flash_mode dio --flash_size 8MB --flash_freq 80m `
    0x0 $bootloader `
    0x8000 $partition `
    0x10000 $appBin

if ($LASTEXITCODE -eq 0) {
    Write-Host "Flashing to $Port completed successfully!" -ForegroundColor Green
} else {
    Write-Error "Flashing failed with exit code $LASTEXITCODE"
}
