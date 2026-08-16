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
- **Target**: ESP32-C6 (ESP-IDF v5.5+)
- **Function**: Auracast Broadcast Sink receiving LC3 audio streams from Android/transmitters.
- **Audio Output**: I2S DAC (e.g. PCM5102A / MAX98357A) with optional DSP biquad filtering.
- **Display**: ST7789 SPI LCD console displaying connection state, RSSI, and diagnostics.
- **Maintenance**: Background Wi-Fi station and HTTP Over-The-Air (OTA) firmware upgrade manager.

### 2. [Node-to-Node Broadcaster (`apps/node2node`)](apps/node2node/README.md)
- **Target**: ESP32-C6 (ESP-IDF v5.5+)
- **Function**: Auracast Broadcast Source (BIG/BIS) streaming unencrypted LC3 audio packets.
- **Audio Source**: I2S digital audio input / built-in multi-frequency sine wave tone generator.
- **Display & Telemetry**: ST7789 SPI color display and addressable RGB WS2812 status LED.

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

This project is licensed under the GNU Affero General Public License v3.0 (AGPL-3.0). See [LICENSE](LICENSE) for details.
