# ESP32-C6 Dual-Node BLE 5.3 LE Audio (Auracast) & GATT Control Architecture

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL_3.0-blue.svg)](../../LICENSE)
[![Target: ESP32-C6](https://img.shields.io/badge/ESP--IDF-v5.2-red.svg)](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32c6/index.html)
[![Bluetooth: 5.3](https://img.shields.io/badge/Bluetooth-5.3_LE_Audio-brightgreen.svg)](https://www.bluetooth.com/specifications/specs/)
[![Codec: LC3](https://img.shields.io/badge/Codec-LC3_Fixed--Point-orange.svg)](https://www.bluetooth.com/specifications/specs/low-complexity-communication-codec-1-0/)

A complete, production-grade **Bluetooth 5.3 LE Audio (Auracast) Broadcast and Bidirectional GATT Control System** implemented on the single-core RISC-V **ESP32-C6** microcontroller using the Apache NimBLE stack.

This architecture decouples high-efficiency **connectionless Broadcast Isochronous Streams (BIS)** for lossless real-time audio distribution from an official Bluetooth SIG standard **GATT Control Plane** (PACS, BASS, VCS) for bidirectional orchestration, telemetry, and per-node volume control.

---

## 1. System Architecture Overview

```
+---------------------------------------------------------------------------------------------------+
|                                     BLE 5.3 AUDIO & GATT CONTROL                                  |
|                                                                                                   |
|    +-----------------------------+                           +-----------------------------+      |
|    |      Node21 (SOURCE)        |                           |       Node20 (SINK)         |      |
|    |  ESP32-C6-WROOM-1 DevKit    |                           |    Waveshare ESP32-C6-LCD   |      |
|    +--------------+--------------+                           +--------------+--------------+      |
|                   |                                                         |                     |
|                   | >>>>>>>> 1. BROADCAST ISOCHRONOUS STREAM (BIS) >>>>>>>>>|                     |
|                   |      (44.1 kHz, 16-bit Mono, 10 ms LC3 Frame, 64 kbps)  |                     |
|                   |      (Periodic Advertising + BigInfo Sync)              |                     |
|                   |                                                         |                     |
|                   | <<<<<<<< 2. BIDIRECTIONAL GATT CONTROL PLANE <<<<<<<<<  |                     |
|                   |      - PACS (0x184E): Sink Audio Capabilities & PAC     |                     |
|                   |      - BASS (0x184F): Broadcast Audio Scan Service      |                     |
|                   |      - VCS  (0x1844): Volume Control Service & Feedback |                     |
|                   |      - GAP Appearance: Stereo Headphones (0x0841)       |                     |
|                   |                                                         |                     |
|    +--------------v--------------+                           +--------------v--------------+      |
|    | 0.10 Hz Sine LFO Orchestrator|                           | Real-Time Hardware Volume    |      |
|    | - 4 Hz VCS Volume Updates   |                           | - PCM Sample Gain Scaling   |      |
|    | - Up to 9 Tracked SINKs     |                           | - MAX98357A I2S Master DAC   |      |
|    +--------------+--------------+                           +--------------+--------------+      |
|                   |                                                         |                     |
|    +--------------v--------------+                           +--------------v--------------+      |
|    | 1 Hz USB Serial Telemetry   |                           | 4 Hz ST7789 LCD Console     |      |
|    | - CPU Mean/Peak, Free Heap  |                           | - 5-sec Mean/Peak CPU Load  |      |
|    | - Tracked SINK Table & Ages |                           | - Live VCS Volume & RSSI    |      |
|    +-----------------------------+                           +-----------------------------+      |
+---------------------------------------------------------------------------------------------------+
```

### Hardware Node Profiles

| Identifier | Target Role | Hardware Board | Physical Interfaces | Default COM Port |
| :--- | :--- | :--- | :--- | :--- |
| **Node21** | **Audio SOURCE** (Broadcaster + Central) | ESP32-C6-WROOM-1 DevKit | USB Serial JTAG + WS2812B RGB LED | **COM21 / COM22** |
| **Node20** | **Audio SINK** (Receiver + Server) | Waveshare ESP32-C6-LCD | 1.47" ST7789 SPI LCD + WS2812B RGB LED + MAX98357A I2S DAC | **COM20** |
| **Node22 / Node23** | **Audio SINK Prototype** (Receiver + Server) | Heemol ESP32-C6 Mini (Zero 20-Pin) | USB Serial JTAG + MAX98357A I2S DAC Module (3.2W Mono Amp) | Custom / Auto |

---

## 2. Key Features

### A. Official Bluetooth SIG LE Audio GATT Services
* **PACS (`0x184E` - Published Audio Capabilities Service):**
  * Exposes `Sink PAC` (`0x2BC9`) specifying LC3 codec capability at 44.1 kHz / 48 kHz, 7.5 ms / 10 ms frame durations, and 20–120 octets/frame.
  * Exposes `Sink Audio Locations` (`0x2BCA`) specifying Front Left + Front Right stereo capabilities (`0x00000003`).
  * Exposes `Sink Audio Contexts` (`0x2BCE`) specifying Media context (`0x0004`).
* **BASS (`0x184F` - Broadcast Audio Scan Service):**
  * Exposes `Broadcast Receive State` (`0x2BC8`, Read & Notify) and `BASS Control Point` (`0x2BC7`, Write).
  * Fully handles standard opcodes: `Add Source` (`0x02`), `Modify Source` (`0x03`), and `Remove Source` (`0x05`).
  * Enables the Central SOURCE node to delegate and command the SINK to synchronize to its active `Broadcast_ID` (`0x123456`) and BIS index.
* **VCS (`0x1844` - Volume Control Service):**
  * Exposes `Volume State` (`0x2B7D`, Read & Notify), `Volume Control Point` (`0x2B7E`, Write), and `Volume Flags` (`0x2B7F`).
  * Fully handles `Set Absolute Volume` (`0x04`), relative volume steps, and mute/unmute commands.
  * SINK automatically pushes instant notification updates back to the SOURCE whenever volume changes.
* **Standard GAP Appearance:**
  * SINK advertises standard Bluetooth SIG Appearance `0x0841` (Stereo Wireless Headphones / LE Audio Sink).

### B. Multi-SINK Orchestration & 0.10 Hz Sine LFO Volume Modulation
* **Multi-Node Tracking:** SOURCE actively tracks up to **9 SINK nodes** simultaneously in a dynamic registry with MAC addresses, connection handles, volume percentages, BASS sync states, and timestamp ages.
* **0.10 Hz Sine LFO Volume Wave:** A dedicated FreeRTOS task on SOURCE runs at **4 Hz** (every 250 ms), generating a 0.10 Hz sine wave (10-second period) in the 10% to 50% volume range (~25 to 128 units out of 255) and transmitting `Set Absolute Volume` (`0x04`) via VCS to all connected SINKs.
* **Direct Hardware Audio Gain Scaling:** Decoded 16-bit PCM audio samples are scaled in real time by `(sample * volume_setting) / 255` directly before DMA transfer to the MAX98357A I2S DAC.

### C. Automatic Reconnection & Advertising Recovery
* **Self-Healing GATT Control Plane:**
  * When a GATT link drops, SINK instantly restarts its connectable extended advertisement instance (`Instance 1`) and SOURCE resumes active discovery.
  * Upon SOURCE reboot, GATT connection, service discovery, BASS stream delegation, and VCS modulation resume automatically in <1 second.
* **0.5 s BIS Loss Detection:** If no broadcast audio frames are received within **500 ms (0.5 s)**, SINK automatically flags sync loss and transitions to `SCANNING` to re-acquire the stream.

### D. Advanced Diagnostic Telemetry & Display
* **5-Second CPU Load Sliding Window:**
  * Calls `calculateCpuUsagePct()` every **500 ms** and maintains a **10-element ring buffer** (10 x 500 ms = 5.0 s).
  * Computes and displays the true **5-second Mean** and **5-second Peak** CPU load.
* **4 Hz ST7789 LCD Console Display (Node20):**
  * Line 0: `NODE: SINK | UP: xx s`
  * Line 1: `CPU: %2d-%2d%% | %2d C | %3lu KB` (5-sec mean-peak load, CPU temp, free DRAM)
  * Line 2: `BT: STREAMING | %+02d dBm | %.1f kpkts` (Live signed RSSI and packet count)
  * Line 3: `BIS: #1 @ ESP32-C6-21` (Synchronized broadcast stream & source name)
  * Line 4: `AUDIO: Mono 16-bit 44.1 kHz` (Stream status)
  * Line 5: `CODEC: LC3 fixp @ 64 kbps` (Audio codec profile)
  * Line 6: `VOL: %3u%% | DAC: OK` (Live VCS volume percentage and DAC status)
* **Convenience Debug Helpers:**
  * `printGATTcommand()` and `printGATTnotification()` output clean, human-readable logs of all BASS and VCS packet transfers.

---

## 3. Directory Layout

```
apps/node2node/
├── CMakeLists.txt                      # Top-level ESP-IDF CMake configuration (Target: esp32c6)
├── partitions.csv                      # Custom 8MB flash partition table
├── sdkconfig.defaults                  # SDK defaults (NimBLE BT 5.3, FreeRTOS stats, Dual Console)
├── README.md                           # System architecture documentation
└── main/
    ├── CMakeLists.txt                  # Component build script (esp_lcd, esp-dsp, led_strip)
    ├── idf_component.yml               # ESP Component Manager dependencies
    ├── config.h                        # Centralized system constants, GATT UUIDs & role selection
    ├── config.c                        # Runtime system configuration structure
    ├── lc3_codec.hpp / .cpp            # Fixed-Point LC3 Audio Encoder & Decoder engine
    ├── tone_generator.hpp / .cpp       # 440 Hz VCO modulated by 0.5-2.0 Hz randomized VFO
    ├── ble_audio_broadcast.hpp / .cpp  # NimBLE BIS Broadcast, GATT Server/Client & VCS LFO
    ├── i2s_audio.hpp / .cpp            # MAX98357A I2S Master TX driver (driver/i2s_std.h)
    ├── lcd_display.hpp / .cpp          # Waveshare 1.47" ST7789 LCD (SPI) & WS2812 RGB LED driver
    ├── diagnostics.hpp / .cpp          # 4 Hz Telemetry task with 5-sec CPU load ringbuffer
    └── main.cpp                        # Application entry point (app_main)
```

---

## 4. Hardware Pinout & Wiring Guides

### A. Node20 (Waveshare ESP32-C6-LCD with Integrated ST7789 & MAX98357A)

| Interface | Signal | ESP32-C6 GPIO | Notes |
| :--- | :--- | :--- | :--- |
| **I2S DAC** | `BCLK` | **GPIO 16** | Bit Clock |
| **I2S DAC** | `WS` | **GPIO 17** | Word Select / Frame Clock |
| **I2S DAC** | `DOUT` | **GPIO 18** | Serial Data Out |
| **ST7789 LCD** | `MOSI` | **GPIO 6** | SPI Data |
| **ST7789 LCD** | `SCLK` | **GPIO 7** | SPI Clock |
| **ST7789 LCD** | `CS` | **GPIO 14** | Chip Select |
| **ST7789 LCD** | `DC` | **GPIO 15** | Data / Command |
| **ST7789 LCD** | `RST` | **GPIO 21** | Reset |
| **ST7789 LCD** | `BL` | **GPIO 22** | Backlight Control |
| **RGB LED** | `DATA` | **GPIO 8** | Onboard WS2812B RGB LED |

---

### B. Node22 / Node23 Audio SINK Prototypes: Heemol ESP32-C6 Mini + MAX98357A I2S DAC

#### Supported Hardware Modules:
* **Microcontroller Module**: [Heemol ESP32-C6 Mini 20-Pin Thumb-Size Dev Board](https://www.amazon.se/dp/B0H33M4Y9R) (ESP32-C6-Zero form factor, 160 MHz RISC-V 32-bit CPU, 2.4 GHz Wi-Fi 6, Bluetooth 5.3 LE, USB-C native Serial/JTAG).
* **Audio Amplifier / DAC Module**: [MAX98357A I2S 3.2W Class-D Mono Amplifier](https://www.aliexpress.com/item/1005012453004931.html) ([Alternative AliExpress Source](https://www.aliexpress.com/item/1005010273388760.html)).

#### Wiring Diagram:

```
+------------------------------------+              +------------------------------------+
|       Heemol ESP32-C6 Mini         |              |      MAX98357A I2S DAC Module      |
|           (20-Pin Module)          |              |          (7-Pin Breakout)          |
|                                    |              |                                    |
|   [ 5V / VBUS ] -------------------|------------> | [ VIN / VDD ] (2.5V - 5.5V Power)  |
|   [ GND ]       -------------------|------------> | [ GND ]       (Common Ground)      |
|   [ GPIO 16 ]   -------------------|------------> | [ BCLK ]      (Continuous Bit Clk) |
|   [ GPIO 17 ]   -------------------|------------> | [ LRC / WS ]  (Left/Right Clock)   |
|   [ GPIO 18 ]   -------------------|------------> | [ DIN / SD ]  (Serial Audio Data)  |
|                                    |              |                                    |
|   (Unconnected) -------------------|------------> | [ GAIN ]      (Float = 9 dB Gain)  |
|   [ 3.3V / GPIO 19 ] --------------|------------> | [ SD_MODE ]   (High = Left Channel)|
|                                    |              |                                    |
|                                    |              |   [ SPK+ / SPK- ] -> 4-8 Ohm Spkr  |
+------------------------------------+              +------------------------------------+
```

#### Detailed Pin Mapping & Signal Descriptions:

| MAX98357A Pin | Pin Function | ESP32-C6 Mini Pin | Connection Details & Configuration |
| :--- | :--- | :--- | :--- |
| **`VIN` / `VDD`** | Amplifier Power Supply | **`5V` (VBUS)** | Connect to the **`5V`** rail for maximum output power (**3.2W into 4Ω** or **1.7W into 8Ω** with high dynamic range and low distortion). Alternatively, connect to **`3.3V`** if 5V is unavailable (~0.7W output). |
| **`GND`** | Power & Signal Ground | **`GND`** | Common ground reference with the ESP32-C6 Mini. |
| **`BCLK`** | I2S Continuous Bit Clock | **`GPIO 16`** | Clock line for PCM bit serialization ($F_s \times 2 \times 16 = 1.4112\text{ MHz}$). |
| **`LRC` / `WS`** | I2S Word Select / Frame Sync | **`GPIO 17`** | Word Select clock running at audio sample rate ($44.1\text{ kHz}$). High = Right, Low = Left. |
| **`DIN` / `SD`** | I2S Serial PCM Data Input | **`GPIO 18`** | Serial audio data stream directly driven by the ESP32-C6 I2S Master TX DMA channel. |
| **`GAIN`** | Gain Setting | **NC (Floating)** | **Leave Unconnected / Floating** for default **9 dB gain** (optimal for standard 4Ω–8Ω mini speakers). <br>• Tied to `GND`: **6 dB** (for high-sensitivity headphones)<br>• 100kΩ to `GND`: **3 dB**<br>• Tied to `VDD`: **12 dB**<br>• 100kΩ to `VDD`: **15 dB** |
| **`SD_MODE`** | Shutdown & Channel Selection | **`3.3V` / `GPIO 19`** | • **Left Channel (Default)**: Connect to **`3.3V`** or leave floating if breakout has an onboard pull-up resistor ($>1.4\text{V}$).<br>• **Software Mute / Power Saving**: Connect to **`GPIO 19`** (HIGH = Active, LOW = Low-power Shutdown $<1\,\mu\text{A}$).<br>• **Right Channel**: Connect via a 100kΩ resistor divider to VDD ($0.77\text{V} - 1.4\text{V}$). |

---

### C. Pin Collision Avoidance Analysis (ESP32-C6 Single-Core RISC-V)

To guarantee flawless boot stability, native USB-C flashing, and peripheral integrity, the following pin constraints were designed into the pinout:

| Reserved / Critical ESP32-C6 Function | GPIOs | Why Avoided for Audio DAC |
| :--- | :--- | :--- |
| **Boot Strapping & Onboard RGB LED** | `GPIO 8` | Controls internal chip boot mode during power-up and drives the onboard WS2812 RGB status LED. |
| **Boot Mode Selection Button** | `GPIO 9` | Physical BOOT button and download mode strapping. External load on reset could force bootloader ROM mode. |
| **Native USB Serial JTAG** | `GPIO 12` (`USB_D-`), `GPIO 13` (`USB_D+`) | Dedicated native USB differential lines for flashing, debugging, and serial terminal over USB-C. |
| **Hardware JTAG Debugging** | `GPIO 4`, `GPIO 5`, `GPIO 14`, `GPIO 15` | Dedicated MTMS, MTDI, MTCK, MTDO pins. Kept unassigned to maintain OpenOCD JTAG debugging capabilities. |
| **Safe Dedicated Audio I2S Matrix** | **`GPIO 16`, `GPIO 17`, `GPIO 18`, `GPIO 19`** | **100% safe general-purpose I/O**. Completely free of strapping constraints, allowing reliable cold boot, runtime DMA streaming, and identical pin assignment across all SINK profiles. |

---

## 5. Build and Flashing Instructions

### PowerShell Environment Setup
```powershell
$env:IDF_TOOLS_PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools"
$env:IDF_PYTHON_ENV_PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env"
$env:PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools\tools\riscv32-esp-elf\esp-13.2.0_20230928\riscv32-esp-elf\bin;C:\Users\stefa\OneDrive\Documents\ESP\.esptools\tools\cmake\3.24.0\bin;C:\Users\stefa\OneDrive\Documents\ESP\.esptools\tools\ninja\1.11.1;C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env\Scripts;" + $env:PATH
```

### 1. Build and Flash Audio SINK (Node20 on COM20)
1. Ensure `#define CONFIG_ACTIVE_NODE_ROLE NODE_ROLE_SINK` in `main/config.h`.
2. Compile and flash:
```powershell
cd c:\Git_ble_audio\apps\node2node
idf.py build
python -m esptool --chip esp32c6 -p COM20 -b 460800 --before default_reset --after hard_reset write_flash 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\esp32c6_ble_audio_broadcast.bin
```

### 2. Build and Flash Audio SOURCE (Node21 on COM21)
1. Set `#define CONFIG_ACTIVE_NODE_ROLE NODE_ROLE_SOURCE` in `main/config.h`.
2. Compile and flash:
```powershell
cd c:\Git_ble_audio\apps\node2node
idf.py build
python -m esptool --chip esp32c6 -p COM21 -b 460800 --before default_reset --after hard_reset write_flash 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\esp32c6_ble_audio_broadcast.bin
```

---

## 6. Live Serial & Telemetry Trace

```text
[NODE21_SRC]  I (12475) SINKS: === Tracked SINK Nodes (1 / 9 max) ===
[NODE21_SRC]  I (12482) SINK_NODE:   [1] 'ESP32-C6-20' | ConnHandle: 1 | Vol: 49.0% (125/255) | BASS: CONNECTED | Age: 58 ms
[NODE21_SRC]  I (12587) NimBLE: GATT procedure initiated: write; att_handle=28 len=2
[NODE20_SINK] I (31465) GATT_CMD: [VCS 0x2B7E Write: SET_ABSOLUTE_VOLUME] Target: 49.8% (127/255)
[NODE20_SINK] I (31465) NimBLE: GATT procedure initiated: notify; att_handle=25
[NODE21_SRC]  I (12667) GATT_NOTIF: [VCS 0x2B7D Notification] Volume: 49.8% (127/255) | Mute: UNMUTED | Counter: #88
[NODE20_SINK] I (31528) : ===== [ESP32-C6-20] heartbeat #31 | Uptime: 31 s =====
[NODE20_SINK] I (31529) SYS: CPU: 11-14% @ 160 MHz | Temp: 27 C | Heap: 231 KB
[NODE20_SINK] I (31532) BT: Role: SINK (Receiver) | State: STREAMING | Pkts: 1205 | RSSI: -41 dBm | BIS ID: 1
[NODE20_SINK] I (31541) AUDIO: Codec: LC3 fixp @ 64 kbps | Mono 16-bit 44.1 kHz | VCS Vol: 49% (127/255 UNMUTED)
```

---

## License

This project is licensed under the [GNU Affero General Public License Version 3 (AGPL-3.0)](../../LICENSE).
