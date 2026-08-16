---
name: esp32-usb-serial-debug
description: >-
  Procedures, scripts, and commands for flashing (uploading firmware) and debugging ESP32/ESP32-C6 devices via USB Virtual Serial Port (USB Serial JTAG / UART) on Windows PowerShell. Includes timeout management and error handling for common issues such as Permission Denied (port in use by another subagent/monitor) and connection timeouts.
---

# ESP32 USB Virtual Serial Flashing & Debugging Skill

This skill provides standard operating procedures, automated PowerShell scripts, and reference commands for compiling, flashing, and monitoring ESP32-C6 boards over USB Virtual Serial (Native USB Serial/JTAG or UART bridge) in Windows PowerShell with robust timeout and error handling.

---

## Hardware & COM Port Reference

| Node Identifier | Board Description | Target Hardware | Default Port | Role |
| :--- | :--- | :--- | :--- | :--- |
| **Node20** | Waveshare ESP32-C6-LCD-1.47 | ST7789 LCD + WS2812B + MAX98357A I2S DAC | **COM20** | Audio SINK (Receiver + LCD) |
| **Node21** | ESP32-C6-WROOM-1 DevKit | WS2812B RGB LED + VCO/VFO Tone Generator | **COM21** | Audio SOURCE (Broadcaster) |

---

## 1. Environment Setup

Always export the ESP-IDF environment variables in PowerShell before invoking `idf.py` or `esptool.py`:

```powershell
$env:IDF_TOOLS_PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools"
$env:IDF_PYTHON_ENV_PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env"
$env:PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env\Scripts;" + $env:PATH
& "C:\Users\stefa\OneDrive\Documents\ESP\v5.2\esp-idf\export.ps1"
```

---

## 2. Port Detection & Status

List available serial ports and chip details:

```powershell
# Quick COM port list
python -m serial.tools.list_ports

# Read chip info from a specific port (e.g. COM20)
python -m esptool --chip esp32c6 -p COM20 chip_id
```

Or execute the helper script:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/list_esp_ports.ps1
```

---

## 3. Firmware Flashing Procedure

### Method A: Automated Flashing Script with Port Pre-check (Recommended)
The [flash_node.ps1](file:///c:/Git_ble_audio/.agents/skills/esp32-usb-serial-debug/scripts/flash_node.ps1) script pre-checks if the port is busy, verifies build artifacts, and executes `esptool.py` with automatic retries:

```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/flash_node.ps1 -Port COM20 -App node2node
```

### Method B: Fast Multi-Binary Flashing
```powershell
# For node2node (Node20 on COM20 or Node21 on COM21)
python -m esptool --chip esp32c6 -p COM20 -b 460800 --connect-attempts 5 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 apps\node2node\build\bootloader\bootloader.bin 0x8000 apps\node2node\build\partition_table\partition-table.bin 0x10000 apps\node2node\build\esp32c6_ble_audio_broadcast.bin
```

---

## 4. Reading Serial Logs & Telemetry

### Safe Non-Blocking Telemetry Capture (Recommended)
The [read_serial.ps1](file:///c:/Git_ble_audio/.agents/skills/esp32-usb-serial-debug/scripts/read_serial.ps1) script handles port locking, configurable timeouts, and always closes the port in a `finally` block so subagents or flashing tools are never locked out:

```powershell
# Read telemetry from COM20 for 10 seconds (with 1500ms line timeout)
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/read_serial.ps1 -Port COM20 -DurationSeconds 10
```

### Direct PowerShell Snippet with Timeout & Error Handling:
```powershell
$port = "COM20"
$sp = $null
try {
    $sp = New-Object System.IO.Ports.SerialPort $port, 115200, None, 8, one
    $sp.ReadTimeout = 1500
    $sp.WriteTimeout = 1500
    $sp.Open()
    
    $startTime = [DateTime]::UtcNow
    while (([DateTime]::UtcNow - $startTime).TotalSeconds -lt 10) {
        try {
            $line = $sp.ReadLine()
            Write-Output $line
        } catch [System.TimeoutException] {}
    }
}
catch [System.UnauthorizedAccessException] {
    Write-Error "PERMISSION DENIED: $port is currently in use by another subagent, terminal, or process."
}
finally {
    if ($sp -ne $null) {
        if ($sp.IsOpen) { $sp.Close() }
        $sp.Dispose()
    }
}
```

---

## 5. Error Handling & Common Issues

### 1. "Permission Denied" / `UnauthorizedAccessException` / `Access is denied`
- **Root Cause**: Another subagent, IDE extension, PowerShell background task, or external terminal (e.g. PuTTY, Arduino Serial Monitor) currently has the COM port open.
- **Handling**:
  - The scripts automatically detect this error and retry after a short delay.
  - To locate and release the process holding the port in Windows PowerShell:
    ```powershell
    # Check for any running background tasks in Antigravity or open monitor tasks
    # Kill stale python esptool / monitor processes if necessary:
    Get-Process | Where-Object { $_.ProcessName -match "python" -or $_.ProcessName -match "idf_monitor" }
    ```

### 2. "Failed to connect to ESP32: No serial data received" / Connection Timeout
- **Root Cause**: Chip is in deep sleep, bad USB connection, or USB CDC line needs re-enumeration.
- **Handling**:
  - `flash_node.ps1` adds `--connect-attempts 5` and a 2-second retry backoff.
  - For stubborn boards, hold the `BOOT` button on the ESP32 while initiating the flash command.

### 3. USB Serial JTAG CDC Re-enumeration Delay
- With `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, the native USB port will momentarily detach from Windows when hard reset.
- Allow 1-2 seconds after flashing before attempting to open the port for reading.
