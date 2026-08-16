param(
    [Parameter(Mandatory=$false)]
    [string]$Port = "COM20",

    [Parameter(Mandatory=$false)]
    [ValidateSet("node2node", "android2node")]
    [string]$App = "node2node",

    [Parameter(Mandatory=$false)]
    [int]$Baud = 460800,

    [Parameter(Mandatory=$false)]
    [int]$MaxRetries = 2
)

# 1. Setup Environment
$env:IDF_TOOLS_PATH = "C:\Users\stefa\OneDrive\Documents\ESP\.esptools"
$env:IDF_PYTHON_ENV_PATH = "C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env"
$env:PATH = "C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env\Scripts;" + $env:PATH
& "C:\Users\stefa\OneDrive\Documents\ESP\v5.2\esp-idf\export.ps1" | Out-Null

# 2. Check Port Availability
$availablePorts = [System.IO.Ports.SerialPort]::GetPortNames()
if ($availablePorts -notcontains $Port) {
    Write-Error "Target Port $Port is not connected! Available COM ports: $($availablePorts -join ', ')"
    exit 1
}

# 3. Check if Port is Currently Locked by another process / subagent
$testSp = $null
try {
    $testSp = New-Object System.IO.Ports.SerialPort $Port
    $testSp.Open()
    $testSp.Close()
}
catch [System.UnauthorizedAccessException] {
    Write-Error "PERMISSION DENIED: $Port is currently held open by another process, subagent, or serial monitor. Please close the active connection before flashing."
    exit 1
}
catch {
    # Non-fatal warning on pre-check
}
finally {
    if ($testSp -ne $null) {
        $testSp.Dispose()
    }
}

# 4. Check Binary Paths
$appBinName = if ($App -eq "node2node") { "esp32c6_ble_audio_broadcast.bin" } else { "esp32c6_ble_audio_receiver.bin" }
$buildDir = "apps\$App\build"

$bootloader = "$buildDir\bootloader\bootloader.bin"
$partition = "$buildDir\partition_table\partition-table.bin"
$appBin = "$buildDir\$appBinName"

foreach ($file in @($bootloader, $partition, $appBin)) {
    if (-not (Test-Path $file)) {
        Write-Error "Build artifact missing: $file. Please compile first via 'idf.py -C apps\$App build'."
        exit 1
    }
}

# 5. Flash with Retry Logic
$success = $false
$attempt = 0

while (-not $success -and $attempt -lt $MaxRetries) {
    $attempt++
    Write-Host "`n[Attempt $attempt/$MaxRetries] Flashing $App firmware to $Port ($Baud baud)..." -ForegroundColor Cyan

    python -m esptool --chip esp32c6 -p $Port -b $Baud --connect-attempts 5 --before default_reset --after hard_reset write_flash `
        --flash_mode dio --flash_size 8MB --flash_freq 80m `
        0x0 $bootloader `
        0x8000 $partition `
        0x10000 $appBin

    if ($LASTEXITCODE -eq 0) {
        $success = $true
        Write-Host "`n[SUCCESS] Flashing $App to $Port completed successfully!" -ForegroundColor Green
    } else {
        Write-Warning "Flashing failed on attempt $attempt (Exit code: $LASTEXITCODE)."
        if ($attempt -lt $MaxRetries) {
            Write-Host "Waiting 2 seconds before retry..." -ForegroundColor Yellow
            Start-Sleep -Seconds 2
        }
    }
}

if (-not $success) {
    Write-Error "Flashing failed after $MaxRetries attempts. If 'Permission Denied', check for open serial monitors. If 'Failed to connect', verify cable connection or hold Boot button while plugging in."
    exit 1
}
