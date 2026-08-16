---
name: esp32-usb-serial-debug
description: >-
  Procedures, scripts, and commands for flashing (uploading firmware) and debugging ESP32/ESP32-C6/ESP32-S3 devices via USB Virtual Serial Port (USB Serial JTAG / UART) on Windows PowerShell. Includes port locking mitigation, lingering process termination, programmatic USB CDC resets, interactive test simulation, and timeout management.
---

# ESP32 USB Virtual Serial Flashing & Debugging Skill

This skill provides comprehensive operating procedures, automated PowerShell scripts, and reference commands for compiling, flashing, resetting, and monitoring ESP32 / ESP32-C6 / ESP32-S3 boards over USB Virtual Serial (Native USB Serial/JTAG or UART bridge) in Windows PowerShell.

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

## 2. Serial Port Management & Pre-Flash Check

Before attempting to flash, ensure no background serial monitors or active Python scripts are locking the target COM port handle (`PermissionError(13, 'Access is denied.')`).

### A. List Active COM Ports with Descriptions
```powershell
& "C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env\Scripts\python.exe" -c "import serial.tools.list_ports; print('\n'.join([p.device + ' - ' + p.description for p in serial.tools.list_ports.comports()]))"
```
Or run the helper:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/list_esp_ports.ps1
```

### B. Terminate Lingering Serial Tasks & Locked Handles
On Windows, pySerial handles can get locked in lingering background processes. Clear them before flashing or initiating new logs:

```powershell
# Targeted shutdown of lingering serial monitor processes (prevents killing toolchains/compilers):
Get-CimInstance Win32_Process -Filter "CommandLine LIKE '%device monitor%' OR CommandLine LIKE '%idf_monitor%'" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

### C. Handle USB Re-Enumeration Delay
When the onboard USB Serial JTAG / USB CDC controller resets during flashing, Windows unbinds and re-binds the virtual COM port.
- Allow **3 seconds** after reset before attempting to reopen serial connections.
- Always use `--before default_reset` and `--connect-attempts 10` in `esptool.py`.

---

## 3. Firmware Flashing Procedure

### Method A: Automated Flashing Script with Port Pre-check (Recommended)
The [flash_node.ps1](file:///c:/Git_ble_audio/.agents/skills/esp32-usb-serial-debug/scripts/flash_node.ps1) script verifies port availability, terminates lingering monitor locks if needed, checks build artifacts, and executes `esptool.py` with 10 connect attempts and automatic retries:

```powershell
# Flash node2node to Node20 (COM20)
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/flash_node.ps1 -Port COM20 -App node2node

# Flash node2node to Node21 (COM21)
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/flash_node.ps1 -Port COM21 -App node2node
```

### Method B: Fast Multi-Binary Flashing
```powershell
# For node2node on COM20
python -m esptool --chip esp32c6 -p COM20 -b 460800 --connect-attempts 10 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 apps\node2node\build\bootloader\bootloader.bin 0x8000 apps\node2node\build\partition_table\partition-table.bin 0x10000 apps\node2node\build\esp32c6_ble_audio_broadcast.bin
```

---

## 4. Programmatic Node Reset (USB CDC / Serial JTAG)

Native USB CDC ports on ESP32-C6 / ESP32-S3 disconnect and re-enumerate upon reset. To reset safely without crashing serial handles:
1. Toggle DTR / RTS lines.
2. Close the port handle immediately before Windows tears down the endpoint.
3. Poll until Windows re-enumerates the COM port (allowing 3 seconds).

Execute via helper script:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/reset_node.ps1 -Port COM20
```

---

## 5. Interactive Simulation & Automated Testing

Instead of requiring physical interaction, send commands over the open serial connection (e.g. `p\n` to simulate button clicks, frequency adjustments, or mode switches) to automate verification tests:

```powershell
# Send command 'p' to COM20 and capture response for 5 seconds
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/send_serial_cmd.ps1 -Port COM20 -Command "p" -ListenSeconds 5
```

---

## 6. Reading Serial Logs & Telemetry

### Safe Non-Blocking Telemetry Capture
The [read_serial.ps1](file:///c:/Git_ble_audio/.agents/skills/esp32-usb-serial-debug/scripts/read_serial.ps1) script handles port locking, configurable timeouts, and always closes the port in a `finally` block so subagents or flashing tools are never locked out:

```powershell
# Read telemetry from COM20 for 10 seconds (with 1500ms line timeout and auto-retry)
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/read_serial.ps1 -Port COM20 -DurationSeconds 10
```

---

## 7. Troubleshooting Matrix

| Issue | Root Cause | Resolution |
| :--- | :--- | :--- |
| **`PermissionError(13, 'Access is denied')`** | COM port held open by stale Python process, subagent, or monitor | Run `Get-CimInstance Win32_Process -Filter "CommandLine LIKE '%device monitor%' OR CommandLine LIKE '%idf_monitor%'" \| ForEach-Object { Stop-Process -Id $_.ProcessId -Force }` |
| **`Failed to connect to ESP32: No serial data received`** | Board in deep sleep or missed bootloader window | Ensure `--connect-attempts 10` is used. If persistent, hold `BOOT` button during plugin. |
| **USB CDC Port Disappears after Reset** | Windows USB re-enumeration takes 1–3 seconds | Allow a 3-second sleep after hard reset before issuing read/write commands. |
| **Garbage characters on monitor** | Baud rate mismatch | Default baud rate for boot & logs on ESP32-C6 USB Serial JTAG is **115200**. |
