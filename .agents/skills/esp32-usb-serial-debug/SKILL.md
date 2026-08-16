---
name: esp32-usb-serial-debug
description: >-
  Procedures, scripts, and commands for flashing (uploading firmware) and debugging ESP32/ESP32-C6 devices via USB Virtual Serial Port (USB Serial JTAG / UART) on Windows PowerShell. Use when the user asks to flash, upload, monitor, or read serial logs from ESP32 nodes (e.g. Node20 on COM20, Node21 on COM21).
---

# ESP32 USB Virtual Serial Flashing & Debugging Skill

This skill provides standard operating procedures, automated PowerShell scripts, and reference commands for compiling, flashing, and monitoring ESP32-C6 boards over USB Virtual Serial (Native USB Serial/JTAG or UART bridge) in Windows PowerShell.

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

### Method A: Fast Multi-Binary Flashing (Recommended)
Use `esptool.py` at 460800 baud with hardware auto-reset for fast flashing:

```powershell
# For node2node (Node20 on COM20 or Node21 on COM21)
python -m esptool --chip esp32c6 -p COM20 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 apps\node2node\build\bootloader\bootloader.bin 0x8000 apps\node2node\build\partition_table\partition-table.bin 0x10000 apps\node2node\build\esp32c6_ble_audio_broadcast.bin

# For android2node (Node20 on COM20)
python -m esptool --chip esp32c6 -p COM20 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 apps\android2node\build\bootloader\bootloader.bin 0x8000 apps\android2node\build\partition_table\partition-table.bin 0x10000 apps\android2node\build\esp32c6_ble_audio_receiver.bin
```

### Method B: Flash Script Helper
Use the automated flashing helper:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/flash_node.ps1 -Port COM20 -App node2node
```

### Method C: IDF Native Flash
```powershell
idf.py -C apps\node2node -p COM20 flash
```

---

## 4. Reading Serial Logs & Telemetry

### Non-Blocking Telemetry Capture in PowerShell (Automated)
To read a clean stream of logs for automated inspection without locking up the terminal:

```powershell
$port = new-object System.IO.Ports.SerialPort "COM20", 115200, None, 8, one
$port.Open()
$port.ReadTimeout = 1500
$startTime = [DateTime]::UtcNow
try {
    while (([DateTime]::UtcNow - $startTime).TotalSeconds -lt 10) {
        try {
            $line = $port.ReadLine()
            Write-Output $line
        } catch [System.TimeoutException] {}
    }
} finally {
    $port.Close()
}
```

Or execute the helper script:
```powershell
powershell -ExecutionPolicy Bypass -File .agents/skills/esp32-usb-serial-debug/scripts/read_serial.ps1 -Port COM20 -DurationSeconds 10
```

---

## 5. Troubleshooting & Best Practices

1. **Access Denied Error on Flash**:
   - Cause: Another process (or a previous PowerShell script) has `COM20` / `COM21` open.
   - Fix: Ensure serial monitor instances or serial ports are closed before issuing `write_flash`.
2. **USB Serial JTAG Disconnects**:
   - With `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, the USB CDC port will briefly detach and re-enumerate upon hard reset. Allow 1-2 seconds after reset before opening the port.
3. **Double Check Flash Size & Mode**:
   - All ESP32-C6 boards in this repository use **8MB Flash (`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`)** with **DIO mode @ 80MHz**.
