# ESP32-C6 BLE Audio Broadcast Firmware (Bluetooth 5.3)

This directory contains the production-grade, unified ESP-IDF firmware for testing direct **Bluetooth 5.3 LE Audio Broadcast (Auracast / BIS)** streaming between two **ESP32-C6** microcontroller nodes without relying on mobile operating system broadcast assistants.

---

## 1. Scope & Bluetooth Hardware Support

This project strictly targets the **Bluetooth 5.3** specification supported by the **ESP32-C6 silicon radio**. It does **NOT** require or implement Bluetooth 5.4 features.

### BLE Audio & Radio Feature Comparison Matrix

| BLE Audio / Radio Feature | Bluetooth 5.3 support | Bluetooth 5.4 support | ESP32-C6 Hardware Support | Project Implementation Status |
| :--- | :--- | :--- | :--- | :--- |
| **Isochronous Channels (ISOC)** | YES (Mandatory for LE Audio) | YES | **YES (Hardware Silicon Baseband)** | Used for microsecond-synchronized audio frames |
| **Broadcast Isochronous Streams (BIS)** | YES | YES | **YES (Hardware Link Layer)** | Active (1 Mono BIS channel at 44.1 kHz, 16-bit) |
| **Broadcast Isochronous Groups (BIG)** | YES (1–31 BIS streams) | YES | **YES (Hardware Link Layer)** | Active (1 BIG container grouping BIS stream) |
| **Connected Isochronous Streams (CIS)** | YES (Point-to-point) | YES | **YES (Hardware Link Layer)** | Not used (Broadcast scope only) |
| **Periodic Advertising (PA / BIGInfo)** | YES (One-way metadata train) | YES | **YES (Hardware Link Layer)** | Active (Transmits PBP & BASE metadata) |
| **Extended Advertising (EA)** | YES (Up to 251-byte PDUs) | YES | **YES (Hardware Link Layer)** | Active (Presence beacon & BAA UUID `0x1852`) |
| **LC3 Audio Codec Support** | YES | YES | **YES (Fixed-Point Integer)** | Active (64 kbps mono fixed-point encoder/decoder) |
| **Public Broadcast Profile (PBP)** | **Standardized** | **Standardized** | **YES (Software Protocol Stack)** | Active (UUID `0x1851` and UUID `0x1852`) |
| **Connection Subrating** | YES | YES | **YES (Hardware Link Layer)** | Supported by Link Layer |
| **Periodic Adv with Responses (PAwR)** | NO | YES | **NO (Silicon lacks PAwR)** | Out of Scope (Not needed for one-way audio) |
| **Encrypted Advertising Data (EAD)** | NO | YES | **NO (Silicon lacks EAD)** | Out of Scope |
| **LE GATT Security Levels (SLC)** | NO | YES | **NO (BT 5.4 only)** | Out of Scope |

---

## 2. Hardware Nodes & System Topology

```
+---------------------------------------------------------------------------------------------------+
|                                     BLE AUDIO BROADCAST TOPOLOGY                                  |
+---------------------------------------------------------------------------------------------------+
|                                                                                                   |
|  [ Node21: Audio SOURCE (COM21 / COM22) ]               [ Node20: Audio SINK (COM20) ]            |
|  Hardware: ESP32-C6-WROOM-1 DevKit                     Hardware: Waveshare ESP32-C6-LCD-1.47      |
|                                                                                                   |
|  +-------------------------------------+                +--------------------------------------+  |
|  |  440 Hz VCO Tone Synthesizer        |                |  BLE Audio Scanner & Sync            |  |
|  |  (Modulated 220-880 Hz by 0.5-2Hz   |  BLE Broadcast |  (PBP Sync & BIG / BIS Receiver)     |  |
|  |   Randomized VFO @ 30% Amplitude)   |  (44.1kHz BIS) |                                      |  |
|  +------------------+------------------+  ────────────> +-------------------+------------------+  |
|                     |                                                       |                     |
|  +------------------v------------------+                +-------------------v------------------+  |
|  |  Fixed-Point LC3 Encoder (64 kbps)  |                |  Fixed-Point LC3 Decoder (64 kbps)   |  |
|  +------------------+------------------+                +-------------------+------------------+  |
|                     |                                                       |                     |
|  +------------------v------------------+                +-------------------v------------------+  |
|  |  BT 5.3 Ext Adv + Periodic Adv (PBP)|                |  MAX98357A I2S Class-D DAC Driver    |  |
|  |  (BAA UUID 0x1852 + BIG/BIS Stream) |                |  (GPIO 16 BCLK, 17 WS, 18 DOUT)      |  |
|  +------------------+------------------+                +-------------------+------------------+  |
|                     |                                                       |                     |
|  +------------------v------------------+                +-------------------v------------------+  |
|  |  1 Hz USB Serial Diagnostics        |                |  1.47" ST7789 LCD Console + WS2812   |  |
|  |  (Uptime, CPU %, Heap, VCO Freq)    |                |  1 Hz Telemetry (RSSI, Bitrate, Fs)  |  |
|  +-------------------------------------+                +--------------------------------------+  |
|                                                                                                   |
+---------------------------------------------------------------------------------------------------+
```

### Hardware Module Assignment

| Identifier | Target Role | Hardware Board | Physical Interfaces | COM Port(s) |
| :--- | :--- | :--- | :--- | :--- |
| **Node21** | **Audio SOURCE** (Broadcaster) | ESP32-C6-WROOM-1 DevKit | USB Serial JTAG + CH343 UART + WS2812B RGB LED | **COM21 / COM22** |
| **Node20** | **Audio SINK** (Receiver + LCD) | Waveshare ESP32-C6-LCD | 1.47" ST7789 LCD + WS2812B RGB LED + MAX98357A I²S DAC | **COM20** |

---

## 3. Key Architecture Principles

1. **Unified Codebase:**
   - Both Source and Sink nodes run from the same source repository.
   - The active node role and hardware board profile are selected via `CONFIG_ACTIVE_NODE_ROLE` in `main/config.h`.
2. **Wi-Fi Disabled:**
   - `CONFIG_ESP_WIFI_ENABLED=n` is set in `sdkconfig.defaults` to eliminate 2.4 GHz RF contention, co-existence switching delays, and memory overhead.
3. **Fixed-Point ALU Optimization (No Hardware FPU):**
   - The ESP32-C6 uses a single-core 160 MHz RISC-V CPU without a hardware FPU.
   - All LC3 encoding/decoding and audio DSP routines use 32-bit integer arithmetic to prevent `soft-fp` software emulation penalties.
4. **10 ms Isochronous Audio Framing:**
   - Audio is generated, encoded, transmitted, received, and decoded at deterministic 10 ms frame intervals (441 samples/frame @ 44.1 kHz).

---

## 4. Directory Layout & Module Breakdown

```
c:\Git_forestChirp\BLE_audio\ble_audio_node\
├── CMakeLists.txt                      # Top-level ESP-IDF CMake configuration (Target: esp32c6)
├── partitions.csv                      # Custom 8MB flash partition table
├── sdkconfig.defaults                  # SDK defaults (FreeRTOS stats, Dual Console, NimBLE BT 5.3)
├── README.md                           # System architecture documentation
└── main/
    ├── CMakeLists.txt                  # Component build script with requirements (esp_lcd, esp-dsp, led_strip)
    ├── idf_component.yml               # ESP Component Manager dependencies
    ├── config.h                        # Centralized system constants & role switches
    ├── config.c                        # Runtime system configuration structure
    ├── lc3_codec.hpp / .cpp            # Fixed-Point LC3 Audio Encoder & Decoder engine
    ├── tone_generator.hpp / .cpp       # 440 Hz VCO modulated by 0.5-2.0 Hz randomized VFO
    ├── ble_audio_broadcast.hpp / .cpp  # NimBLE Extended Advertising, PBP, and broadcast loop
    ├── i2s_audio.hpp / .cpp            # MAX98357A I²S Master TX driver (driver/i2s_std.h)
    ├── lcd_display.hpp / .cpp          # Waveshare 1.47" ST7789 LCD (SPI) & WS2812 RGB LED driver
    ├── diagnostics.hpp / .cpp          # 1 Hz FreeRTOS telemetry task (USB Serial & LCD)
    └── main.cpp                        # Application entry point (app_main)
```

---

## 5. Component Details

### A. Central Configuration (`config.h` / `config.c`)
- **Role Selection:**
  - `NODE_ROLE_SOURCE (2)`: Configures node as Audio Source (Node21).
  - `NODE_ROLE_SINK (1)`: Configures node as Audio Sink (Node20).
- **Audio Specs:** Sample Rate: 44,100 Hz, Bit Depth: 16-bit, Channels: 1 (Mono), Frame Duration: 10 ms (441 samples/frame), Encoded Frame: 80 octets (64 kbps).
- **I²S Pinout (Node20):** `BCLK` = GPIO 16, `WS` = GPIO 17, `DOUT` = GPIO 18.
- **LCD Pinout (Node20):** `MOSI` = GPIO 6, `SCLK` = GPIO 7, `CS` = GPIO 14, `DC` = GPIO 15, `RST` = GPIO 21, `BL` = GPIO 22, `RGB LED` = GPIO 8.

### B. VCO / VFO Audio Test Tone Synthesizer (`tone_generator.hpp` / `.cpp`)
- **Carrier (VCO):** Nominal 440 Hz sine carrier sweeping between 220 Hz and 880 Hz.
- **Modulator (VFO):** Low-frequency sine modulator running at 0.5 Hz to 2.0 Hz.
- **Cycle-by-Cycle Randomization:** When the VFO phase completes a full electrical cycle ($2\pi$), a new random modulation frequency between 0.5 Hz and 2.0 Hz is selected.
- **Amplitude:** Fixed at 30% of full-scale 16-bit integer range ($32767 \times 0.30 \approx 9830$).

### C. Fixed-Point LC3 Codec (`lc3_codec.hpp` / `.cpp`)
- **Encoder:** Converts 441 raw 16-bit PCM samples into an 80-octet compressed LC3 frame.
- **Decoder:** Decompresses 80-octet LC3 frames back to 441 16-bit PCM samples for I²S DAC transmission.

### D. BLE Audio Broadcast Engine (`ble_audio_broadcast.hpp` / `.cpp`)
- **Source Mode (Node21):**
  - Configures NimBLE Extended Advertising with Public Broadcast Profile (PBP) Service Data (UUID `0x1851`) and Broadcast Audio Announcement (UUID `0x1852`).
  - Runs a dedicated 10 ms FreeRTOS audio task that continuously synthesizes the VCO tone, compresses it via LC3, and broadcasts it over the BIS stream.
- **Sink Mode (Node20):**
  - Runs NimBLE Extended Scanning to discover and synchronize to the Broadcast Audio Announcement.
  - Receives BIS audio frames, decompresses PCM via the LC3 decoder, and writes samples to the MAX98357A I²S DMA queue.

### E. 1 Hz Diagnostic Telemetry & Display (`diagnostics.hpp` / `.cpp` / `lcd_display.hpp`)
- Executes once per second (1000 ms period).
- **Logged Telemetry:** Uptime, CPU Load (%), Free DRAM / Minimum Free Heap, Onboard Temperature Sensor (°C), BT Role & State, Transmitted/Received Packet Counts, RSSI (dBm), BIS Index, Audio Sample Rate, Bit Depth, Channels, Bitrate, and Current Modulated Tone Frequency (Hz).
- **Node20 LCD Dashboard:** Renders formatted color text lines to the 320x172 ST7789 display and sets the WS2812 RGB LED status color (Green = Streaming, Blue = Synced, Orange = Idle).

---

## 6. Build and Flashing Instructions

### Environment Setup (PowerShell)
```powershell
$env:IDF_TOOLS_PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools"
$env:IDF_PYTHON_ENV_PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env"
$env:PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools\tools\riscv32-esp-elf\esp-13.2.0_20230928\riscv32-esp-elf\bin;C:\Users\stefa\OneDrive\Documents\ESP\.esptools\tools\cmake\3.24.0\bin;C:\Users\stefa\OneDrive\Documents\ESP\.esptools\tools\ninja\1.11.1;C:\Users\stefa\OneDrive\Documents\ESP\.esptools\python_env\idf5.2_py3.11_env\Scripts;" + $env:PATH
& "C:\Users\stefa\OneDrive\Documents\ESP\v5.2\esp-idf\export.ps1"
```

### 1. Build and Flash Audio Source (Node21 on COM22)
1. Ensure `#define CONFIG_ACTIVE_NODE_ROLE NODE_ROLE_SOURCE` in `main/config.h`.
2. Compile and flash:
```powershell
idf.py build
esptool.py --chip esp32c6 -p COM22 -b 115200 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\esp32c6_ble_audio_broadcast.bin
```

### 2. Build and Flash Audio Sink (Node20 on COM20)
1. Set `#define CONFIG_ACTIVE_NODE_ROLE NODE_ROLE_SINK` in `main/config.h`.
2. Compile and flash:
```powershell
idf.py build
esptool.py --chip esp32c6 -p COM20 -b 115200 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\esp32c6_ble_audio_broadcast.bin
```

## License

This project is licensed under the [GNU General Public License v3.0 (GPL-3.0)](../../LICENSE).
