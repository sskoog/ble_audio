---
name: esp32-usb-serial-debug
description: >-
  Procedures, scripts, and commands for flashing (uploading firmware) and debugging ESP32/ESP32-C6/ESP32-S3 devices via USB Virtual Serial Port (USB Serial JTAG / UART) on Windows PowerShell using ESP-IDF v6.0.2. Includes port locking mitigation, lingering process termination, programmatic USB CDC resets, interactive test simulation, and timeout management.
---

# ESP32 USB Virtual Serial Flashing & Debugging Skill

This skill provides comprehensive operating procedures, automated PowerShell scripts, and reference commands for compiling, flashing, resetting, and monitoring ESP32 / ESP32-C6 / ESP32-S3 boards over USB Virtual Serial (Native USB Serial/JTAG or UART bridge) in Windows PowerShell.

---

## Hardware & COM Port Reference

| Node Identifier | Board Description | Target Hardware | Default Port | Role |
| :--- | :--- | :--- | :--- | :--- |
| **Node16** | Seeed Studio XIAO ESP32-S3 | ESP32-S3 (Xtensa LX7 + FPU) | **COM16** | Audio SOURCE (Stereo/Mono Broadcaster) |
| **Node20** | Waveshare ESP32-C6-LCD-1.47 | ST7789 LCD + WS2812B | **COM20** | Audio SINK (Receiver + LCD) |
| **Node21** | ESP32-C6-WROOM-1 DevKit | WS2812B RGB LED | **COM21** & **COM121** | Audio SOURCE / USB Host Bridge |
| **Node23** | Waveshare ESP32-C6-Zero | WS2812B RGB LED + MAX98357A I2S DAC | **COM23** | Audio SINK Left (Ch 0) |
| **Node24** | Waveshare ESP32-C6-Zero | WS2812B RGB LED + MAX98357A I2S DAC | **COM24** | Audio SINK Right (Ch 1) |
| **Node25** | Heemol ESP32-C6 Mini | ESP32-C6 (RISC-V) | **COM25** | Audio SINK / Test Node |
| **Node26** | Heemol ESP32-C6 Mini | ESP32-C6 (RISC-V) | **COM26** | Audio SINK / Test Node |

---

## 1. Environment Setup (ESP-IDF v6.0.2)

Always export the ESP-IDF v6.0.2 environment variables in PowerShell before invoking `idf.py` or `esptool.py`:

```powershell
if (Test-Path "C:\Users\stefa\OneDrive\Documents\ESP\.esptools") {
    $env:IDF_TOOLS_PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools"
} else {
    $env:IDF_TOOLS_PATH="C:\Users\stefa\.espressif"
}

if (Test-Path "$env:IDF_TOOLS_PATH\python_env\idf6.0_py3.13_env") {
    $env:IDF_PYTHON_ENV_PATH="$env:IDF_TOOLS_PATH\python_env\idf6.0_py3.13_env"
} else {
    $env:IDF_PYTHON_ENV_PATH="$env:IDF_TOOLS_PATH\python_env\idf6.0_py3.11_env"
}
$env:PATH="$env:IDF_PYTHON_ENV_PATH\Scripts;" + $env:PATH

if (Test-Path "C:\Users\stefa\OneDrive\Documents\ESP\v6.0.2\esp-idf\export.ps1") {
    . "C:\Users\stefa\OneDrive\Documents\ESP\v6.0.2\esp-idf\export.ps1"
} else {
    . "C:\Users\stefa\OneDrive\Documents\ESP\v6.0.2\export.ps1"
}
```

---

## 2. Serial Port Management & Pre-Flash Check

Before attempting to flash, ensure no background serial monitors or active Python scripts are locking the target COM port handle (`PermissionError(13, 'Access is denied.')`).

### A. List Active COM Ports with Descriptions
```powershell
python -c "import serial.tools.list_ports; print('\n'.join([p.device + ' - ' + p.description for p in serial.tools.list_ports.comports()]))"
```

### B. Terminate Lingering Serial Tasks & Locked Handles
On Windows, pySerial handles can get locked in lingering background processes. Clear them before flashing:

```powershell
Get-CimInstance Win32_Process -Filter "CommandLine LIKE '%device monitor%' OR CommandLine LIKE '%idf_monitor%' OR CommandLine LIKE '%test_audio_matrix%' OR CommandLine LIKE '%pc_audio_streamer%'" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

### C. Handle USB Re-Enumeration Delay
When the onboard USB Serial JTAG / USB CDC controller resets during flashing, Windows unbinds and re-binds the virtual COM port.
- Allow **3 seconds** after reset before attempting to reopen serial connections.
- Always use `--before default_reset`, `--after hard_reset`, and `--connect-attempts 10` in `esptool.py`.

---

## 3. Firmware Flashing Procedure

### Method A: Automated Build and Flash
Use the app-specific script:
```powershell
# Flash SOURCE (Node 16 / S3)
powershell -ExecutionPolicy Bypass -File apps\audioESP-NOW\build_and_flash.ps1 -Role SOURCE -Port COM16

# Flash SINK Left (Node 23 / C6)
powershell -ExecutionPolicy Bypass -File apps\audioESP-NOW\build_and_flash.ps1 -Role SINK -Port COM23 -NodeId 23

# Flash SINK Right (Node 24 / C6)
powershell -ExecutionPolicy Bypass -File apps\audioESP-NOW\build_and_flash.ps1 -Role SINK -Port COM24 -NodeId 24
```

### Method B: Fast Multi-Binary Flashing with esptool
```powershell
# For ESP32-C6 on COM23
python -m esptool --chip esp32c6 -p COM23 -b 460800 --connect-attempts 10 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 apps\audioESP-NOW\build_c6\bootloader\bootloader.bin 0x8000 apps\audioESP-NOW\build_c6\partition_table\partition-table.bin 0x10000 apps\audioESP-NOW\build_c6\esp32_espnow_audio.bin
```

---

## 4. Programmatic Node Reset (USB CDC / Serial JTAG)

Native USB CDC ports on ESP32-C6 / ESP32-S3 disconnect and re-enumerate upon reset. To reset safely:
1. Toggle DTR / RTS lines.
2. Close the port handle immediately.
3. Wait 3 seconds for Windows to re-enumerate the COM port.

---

## 5. Reading Serial Logs & Telemetry

Open the port at 115200 baud with a safe timeout and ensure the port is closed cleanly in a `try...finally` block.
