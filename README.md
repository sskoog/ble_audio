# BLE Audio & Auracast Projects

Repository containing firmware applications, hardware documentation, and architectural guides for Bluetooth Low Energy (BLE) Audio and Auracast broadcast/reception using ESP32-C6 / ESP-IDF and dedicated Bluetooth audio SoC solutions.

## Repository Layout

```
.
├── apps/
│   ├── android2node/    # Android-to-Node Auracast receiver with LCD console & OTA
│   └── node2node/       # Node-to-Node Auracast broadcaster with I2S input & LCD UI
├── components/          # Shared ESP-IDF components across applications
├── docs/                # Technical guides, hardware analysis, and feature matrices
└── images/              # Hardware pinouts, board schematics, and module photos
```

## Applications

### 1. [Android-to-Node Receiver (`apps/android2node`)](apps/android2node/README.md)
- **Target**: ESP32-C6 (ESP-IDF v6.0.2)
- **Function**: Auracast Broadcast Sink receiving LC3 audio streams from Android/transmitters.
- **Audio Output**: I2S DAC (e.g. PCM5102A / MAX98357A) with optional DSP biquad filtering.
- **Display**: ST7789 SPI LCD console displaying connection state, RSSI, and diagnostics.
- **Maintenance**: Background Wi-Fi station and HTTP Over-The-Air (OTA) firmware upgrade manager.

### 2. [Node-to-Node Broadcaster (`apps/node2node`)](apps/node2node/README.md)
- **Target**: ESP32-C6 (ESP-IDF v6.0.2)
- **Function**: Auracast Broadcast Source (BIG/BIS) streaming unencrypted LC3 audio packets.
- **Audio Source**: I2S digital audio input / built-in multi-frequency sine wave tone generator.
- **Display & Telemetry**: ST7789 SPI color display and addressable RGB WS2812 status LED.

### 3. [USB BLE Bumble Broadcaster (`apps/usb_ble_bumble`)](apps/usb_ble_bumble/README.md)
- **Target**: ESP32-C6 (ESP-IDF v6.0.2) + Win11 PC with Python 3.13.
- **Function**: Auracast Broadcast Source (BIG/BIS) streaming unencrypted LC3 audio packets from host via USB COM port.
- **Audio Source**: Any PC sound via virtual CABLE input + built-in multi-frequency sine wave tone generator.
- **Telemetry**: Console output on USB COM port.

## Hardware used

### ESP32 Nodes & COM Port Enumeration

Unique hardware nodes in this project are enumerated according to their assigned COM-port IDs on the host machine:

| COM Port(s) | Node Identifier | Hardware Board / Module | Description & Product References |
| :--- | :--- | :--- | :--- |
| **COM20** | **Node20** | **ESP32-C6-LCD** | [Waveshare ESP32-C6-LCD](https://www.amazon.se/dp/B0DHTMYTCY) 18-pin thumb-size module with 1.47" 172x320 px LCD + WS2812B RGB LED |
| **COM21 / COM121** | **Node21** | **ESP32-C6-WROOM** | [ESP32-C6-WROOM-1](https://www.amazon.se/dp/B0CN66P5XY?ref=ppx_yo2ov_dt_b_fed_asin_title) 32-pin full-size module (COM21: Flash & Telemetry, COM121: Bumble & Audio Ingest) + WS2812B RGB LED |
| COM23 | Node23 | **ESP32-C6-zero** | [Waveshare ESP32-C6-Zero](https://www.amazon.se/dp/B0F12PRH9G) 18-pin thumb-size module |
| COM24 | Node24 | **ESP32-C6-zero** | [Waveshare ESP32-C6-Zero](https://www.amazon.se/dp/B0F12PRH9G) 18-pin thumb-size module |
| COM25 | Node25 | **ESP32-C6-mini** | [Heemol ESP32-C6 Mini](https://www.amazon.se/dp/B0H33M4Y9R) 20-pin thumb-size module |
| COM26 | Node26 | **ESP32-C6-mini** | [Heemol ESP32-C6 Mini](https://www.amazon.se/dp/B0H33M4Y9R) 20-pin thumb-size module |

### I2S decoders and power amplifiers
* **Audio Amplifier / DAC Module**: [MAX98357A I2S 3.2W Class-D Mono Amplifier](https://www.aliexpress.com/item/1005012453004931.html) ([Alternative AliExpress Source](https://www.aliexpress.com/item/1005010273388760.html)).

## LC3 Audio Quality
LC3 audio is always encoded per-channel and defined by encoding parameters:
* Frame duration: 7.5 or 10.0 ms
* Sampling rate: [8, 16, 24, 32, 48] kHz
* Packet size: 8 to 320 bytes, in steps of 1 byte.
The combination of encoding parameters yelds effective bitrates from 16 to 320 kbps, where about 96 kbps is considered to be "near-transparent fidelity", matching 320 kbps MP3.

To simplify the choice of parameters in this project, four levels of quality are defined, all assuming a 10 ms frame duration:
| Tier | Sampling Rate (`f_s`) | Frame Payload (`N_octets`) | Bitrate (`R`) | Audio Bandwidth | Target Acoustic Profile |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **VHQ (Very High)** | 48 kHz | 160 bytes | 128 kbps | 20 Hz – 20+ kHz | Reference transparency; dense symphonic & transient fidelity |
| **HQ (High)** | 48 kHz | 120 bytes | 96 kbps | 20 Hz – 20+ kHz | Near-transparent fidelity; matches 320 kbps MP3 |
| **MQ (Medium)** | 32 kHz | 80 bytes | 64 kbps | 20 Hz – 16 kHz | High-efficiency music; comparable to 160–192 kbps MP3 |
| **LQ (Low)** | 24 kHz | 40 bytes | 32 kbps | 20 Hz – 12 kHz | Constrained RF streaming; preserves musical bass and core mix |

### VHQ (Very High Quality) — 48 kHz, 160 Bytes (128 kbps)
- **Spectral Resolution**: Allocates 480 samples per 10 ms frame (`N_F = 480`) with a bit density of 2.67 bits per MDCT spectral bin.
- **Perceptual Performance**: Operates at the threshold of strict perceptual transparency for mono material. While standard pop and electronic tracks reach transparency at lower rates, 128 kbps mono gives the context-adaptive arithmetic coder the headroom needed to encode dense orchestral polyphony, wide dynamic range live recordings, and sharp percussion without subtle LD-MDCT smearing.
- **Best Used For**: Critical listening through resolving studio monitors or open-back headphones reproducing lossless CD or 320 kbps sources.

### HQ (High Quality) — 48 kHz, 120 Bytes (96 kbps)
- **Spectral Resolution**: 480 samples per frame (`N_F = 480`) with 2.0 bits per spectral bin.
- **Perceptual Performance**: Matches the High-Fidelity Broadcast standard. It delivers full high-frequency extension past 20 kHz with no artificial lowpass filtering. In double-blind testing, LC3 at 96 kbps per channel consistently matches or exceeds the subjective fidelity of classic MP3 at 192–320 kbps.
- **Best Used For**: High-fidelity wireless streaming from Spotify (320 kbps Vorbis/AAC) or YouTube Music (256 kbps AAC) under solid RF conditions.

### MQ (Medium Quality) — 32 kHz, 80 Bytes (64 kbps)
- **Spectral Resolution**: 320 samples per frame (`N_F = 320`), yielding 2.0 bits per spectral bin.
- **Perceptual Performance**: Dropping the sampling frequency to 32 kHz sets an anti-aliasing cutoff at 16 kHz. By omitting the highest, psychoacoustically least sensitive half-octave (16–20 kHz), all 80 bytes are dedicated to mid-range vocals, percussion transients, and low-end harmonics. It sounds clean and punchy, avoiding the phase smearing and metallic artifacts that plague legacy codecs at 64 kbps.
- **Best Used For**: Casual listening, multi-room synchronized streaming, or links experiencing moderate wireless congestion.

### LQ (Low Quality) — 24 kHz, 40 Bytes (32 kbps)
- **Spectral Resolution**: 240 samples per frame (`N_F = 240`), allocating 1.33 bits per spectral bin.
- **Selection Rationale**: While 16 kHz is supported in LC3, it is typically reserved strictly for voice applications due to its hard 8 kHz cutoff. Selecting 24 kHz retains a 12 kHz acoustic bandwidth, preserving the full fundamental range of instruments and vocal sibilance. Independent listening tests show that most users remain satisfied with 24 kHz LC3 encoding even under good listening conditions.
- **Best Used For**: Low-power links, fringe RF range, or battery-constrained sensor endpoints where maintaining audio continuity without dropouts takes priority over high-end treble air.

<br>

---


## Documentation & Research

- [Auracast Audio Architecture Guide](docs/auracast_audio.md)
- [Espressif ESP32 BLE Audio Capabilities](docs/espressif_esp32_ble_audio.md)
- [Feasycom FSC-BT1058 BLE Audio Module](docs/fsc_bt1058.md)
- [Nordic nRF5340 BLE Audio Guide](docs/nRF5340_ble_audio.md)
- [ESP32 Feature Matrix Spreadsheet](docs/ESP32-Feature-Matrix-2026.xlsx)

## Prerequisites & Build Setup

1. **ESP-IDF v5.5+** installed and activated in your environment:
   ```bash
   . $IDF_PATH/export.sh   # Linux / macOS
   export.bat              # Windows CMD / PowerShell
   ```

2. **Building an App**:
   ```bash
   cd apps/node2node
   idf.py set-target esp32c6
   idf.py build
   idf.py flash monitor
   ```

## License

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)

This project is licensed under the **GNU Affero General Public License Version 3 (AGPL-3.0)**.

```text
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program. If not, see <https://www.gnu.org/licenses/>.
```

See [LICENSE](LICENSE) for the full license text.
