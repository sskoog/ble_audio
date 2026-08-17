# Bluetooth 5.3 LE Audio: Two-Way Control & Telemetry Architecture

This document details the architecture, standardized profiles, and custom protocols for **two-way (bidirectional) control, parameter configuration, and diagnostic telemetry** between an Audio **SOURCE** (Broadcaster / Broadcast Assistant) and Audio **SINK** (Receiver / Speaker) nodes.

> [!IMPORTANT]
> **Bluetooth Version Delimitation**: This document strictly covers **Bluetooth 5.3** LE Audio specifications. Bluetooth 5.4 features (such as *Periodic Advertising with Responses / PAwR* and *Encrypted Advertising Data / EAD*) are **explicitly out of scope and not considered**.

---

## 1. Architectural Overview: Decoupled Audio & Control Planes

In Bluetooth 5.3 LE Audio (Auracast / Public Broadcast Profile), the architecture cleanly separates **High-Bandwidth Audio Distribution** from the **Low-Latency Bidirectional Control Plane**:

```
                  +----------------------------------------------+
                  |           AUDIO SOURCE / ASSISTANT           |
                  |             (e.g., ESP32-C6 Node21)          |
                  +----------------------------------------------+
                                  /            \
          Connectionless         /              \      Bidirectional
          Isochronous Audio     /                \     Point-to-Point GATT
          Broadcast (BIS)      /                  \    Control Plane
                              v                    v
              +-----------------------+    +-----------------------+
              |    AUDIO SINK #1      |<-->|   AUDIO SINK #2       |
              | (ESP32-C6 Node20 LCD) |    | (ESP32-C6 Satellite)  |
              +-----------------------+    +-----------------------+
                  [Passive Listener]           [Passive Listener]
                  (Unlimited Nodes)            (4-9 Active GATT Links)
```

1. **Audio Data Plane (Connectionless Broadcast)**:
   - Distributed via **Broadcast Isochronous Streams (BIS)** within a Broadcast Isochronous Group (BIG).
   - Audio is encoded in fixed-point **LC3 (Low Complexity Communication Codec)** and broadcast over 2.4 GHz Isochronous physical channels.
   - **Scalability**: Can be received by an **unlimited number of passive SINK nodes** simultaneously without radio bandwidth degradation.
2. **Control & Telemetry Plane (Bidirectional GATT / Beacons)**:
   - Operates in parallel via **BLE GATT (Generic Attribute Profile)** connections or connectionless telemetry advertisements.
   - Enables volume adjustment, mute control, channel assignment, phase/delay alignment, DSP filter tuning, and real-time status feedback from SINK back to SOURCE.

---

## 2. Standardized Bluetooth SIG LE Audio Profiles (GATT)

The Bluetooth Special Interest Group (SIG) defines dedicated GATT services for LE Audio. In this model, the **SINK node acts as a GATT Server**, exposing standardized characteristics that the **SOURCE (or a smartphone Broadcast Assistant)** controls as a GATT Client.

### Profile Summary Matrix

| Service Name | Service UUID | Primary Purpose | Communication Direction | Key Characteristics |
| :--- | :--- | :--- | :--- | :--- |
| **Broadcast Audio Scan Service (BASS)** | `0x184F` | Broadcast stream discovery, source selection, and tuning orchestration. | Bidirectional (SOURCE -> SINK commands, SINK -> SOURCE status) | `0x2BC7` (Control Point)<br>`0x2BC8` (Receive State) |
| **Volume Control Service (VCS)** | `0x1844` | Master volume control, relative stepping, and mute management. | Bidirectional (SOURCE writes volume, SINK notifies changes) | `0x2B7D` (Volume State)<br>`0x2B7E` (Control Point)<br>`0x2B7F` (Volume Flags) |
| **Volume Offset Control Service (VOCS)** | `0x1845` | Channel balance and per-speaker volume offset. | Bidirectional (Adjusts relative balance between Left/Right speakers) | `0x2B80` (Offset State)<br>`0x2B82` (Control Point) |
| **Audio Input Control Service (AICS)** | `0x1843` | Gain mode (manual/automatic), gain level, and input muting. | Bidirectional (Controls analog/digital front-end stages) | `0x2B77` (Input State)<br>`0x2B79` (Control Point) |
| **Published Audio Capabilities Service (PACS)** | `0x184E` | Advertises supported LC3 codecs, sample rates, frame sizes, and channel allocations. | Read-Only / SINK -> SOURCE | `0x2BC9` (Sink PAC)<br>`0x2BCA` (Sink Audio Locations) |

---

## 3. Deep-Dive: Broadcast Audio Scan Service (BASS `0x184F`)

BASS is the core orchestrator for Auracast broadcast reception. It enables a **Broadcast Assistant** (such as a SOURCE node or Android 15/17 smartphone) to manage what a SINK is listening to.

```
  SOURCE / ASSISTANT                                   SINK NODE
          |                                                |
          | ----- 1. GATT Write: Add Source (0x2BC7) ----> | (Sets Broadcast_ID, Subgroup, BIS)
          |                                                |
          | <--- 2. GATT Notify: Receive State (0x2BC8) -- | (PA Synced, Decrypting OK)
          |                                                |
          |                  [Audio Broadcast on BIS]      |
          | =============================================> | (SINK decodes LC3 audio)
          |                                                |
          | <--- 3. GATT Notify: Sync Lost (0x2BC8) ------ | (Reports broadcast signal drop)
```

### A. SINK -> SOURCE Feedback (`0x2BC8` Broadcast Receive State)
The SINK enables **GATT Notifications** on Characteristic `0x2BC8`. Whenever the SINK's reception state changes, it pushes a notification packet to the SOURCE containing:
* **`Source_Address` & `Source_Adv_SID`**: Identifies the transmitter broadcasting the audio.
* **`PA_Sync_State`**: `0x00` (Not Synced), `0x01` (SyncInfo Request), `0x02` (Synchronized to Periodic Advertising).
* **`BIG_Encryption_State`**: `0x00` (Unencrypted), `0x01` (Broadcast Code Required), `0x02` (Decrypting OK), `0x03` (Bad Code).
* **`Subgroup_BIS_Sync`**: A 32-bit bitfield indicating which specific audio channels/BIS indices the node is actively receiving.

### B. SOURCE -> SINK Control (`0x2BC7` BASS Control Point)
The SOURCE issues commands to the SINK via GATT Writes to `0x2BC7`:
* **`Add Source` (`0x02`)**: Instructs the SINK to tune into a specific `Broadcast_ID`, channel index, and subgroup.
* **`Modify Source` (`0x03`)**: Updates channel index (e.g. switch from Left to Right speaker channel) or metadata.
* **`Set Broadcast Code` (`0x04`)**: Provides the 16-byte decryption key for encrypted streams.
* **`Remove Source` (`0x05`)**: Orders the SINK to stop reception and return to idle scanning.

---

## 4. Custom Parameter & Telemetry Exchange over GATT

Beyond standard SIG services, Bluetooth 5.3 allows registering **Custom 128-bit Vendor Services** on the ESP32-C6.

> [!NOTE]
> **UUID Size vs. Data Payload**: The term **"128-bit"** refers solely to the length of the Service Address/UUID (16 bytes) to prevent namespace collisions. The actual data payload capacity per GATT packet is governed by the **ATT MTU (up to 247 bytes per transaction in BLE 5.3)**, with support for unbounded multi-kilobyte transfers via long blob writes.

### A. Parameter Control (SOURCE -> SINK)

Using a custom GATT characteristic (Write Without Response), the SOURCE can transmit structured binary control frames:

```cpp
struct __attribute__((packed)) NodeControlPayload {
    uint8_t  command_id;       // 0x01 = Set DSP, 0x02 = Set Delay, 0x03 = Set UI
    uint8_t  volume_percent;   // 0 - 100% digital gain
    uint16_t hpf_cutoff_hz;    // High-Pass Filter cutoff (e.g. 80 Hz, 120 Hz, Bypass)
    uint8_t  delay_align_ms;   // Per-node audio delay compensation (0 - 100 ms)
    uint8_t  channel_mode;     // 0 = Mono Mix, 1 = Left Only, 2 = Right Only
    uint8_t  led_brightness;   // WS2812B RGB brightness (0 - 100%)
    uint8_t  lcd_backlight;    // ST7789 LCD PWM brightness (0 - 100%)
};
```

1. **Dynamic DSP Tuning**: Modify biquad filter coefficients over-the-air to adapt to speaker acoustic enclosure sizes.
2. **Phase & Delay Alignment**: Introduce small millisecond buffer delays (0 - 100 ms) on closer speakers to eliminate comb filtering and ensure phase-coherent audio arrival.
3. **Channel Routing**: Dynamically re-assign speaker roles without rebooting the microcontroller.

### B. Status & Health Telemetry (SINK -> SOURCE)

Using a custom GATT characteristic (Notifications enabled), each SINK periodically pushes health metrics:

```cpp
struct __attribute__((packed)) NodeTelemetryPayload {
    char     node_name[12];    // e.g. "Node20-LCD"
    uint32_t packets_received; // Total audio frames decoded
    uint16_t packets_lost;     // Frame loss / concealment counter
    int8_t   rssi_dbm;         // BLE radio signal strength (-100 to 0 dBm)
    uint8_t  cpu_usage_pct;    // FreeRTOS CPU busy utilization (0 - 100%)
    int8_t   cpu_temp_c;       // On-chip temperature sensor (deg C)
    uint16_t battery_mv;       // Battery supply voltage (mV)
    uint32_t free_heap_bytes;  // Available internal SRAM (bytes)
};
```

---

## 5. Comparison of Two-Way Feedback Topologies (BT 5.3)

| Metric / Requirement | **Method 1: BASS Receive State (`0x2BC8`)** | **Method 2: Connectionless SINK Beacons** | **Method 3: Custom Vendor GATT Service** |
| :--- | :--- | :--- | :--- |
| **Max Tracked SINK Nodes** | **4 - 9 nodes** (GATT connection limit) | **Unlimited** (Connectionless advertising) | **4 - 9 nodes** (GATT connection limit) |
| **Standard Compliance** | **Official Bluetooth SIG LE Audio** | Custom / Proprietary | Custom Vendor GATT |
| **Android 15/17 Native Interop** | **Full Native Compatibility** | SINK requires custom app | SINK requires custom app |
| **Payload Capacity** | Fixed BASS fields (10-40 bytes) | Up to 251 bytes (Extended Adv PDU) | Up to 247 bytes per GATT packet |
| **Feedback Latency** | Low (< 30 ms over active GATT) | Medium (500 - 2000 ms adv interval) | Low (< 30 ms over active GATT) |
| **Radio Connection Overhead** | Moderate (Maintains BLE connection) | **Zero connection handles required** | Moderate (Maintains BLE connection) |
| **Best Use Case** | Commercial Auracast ecosystems | Large multi-speaker arrays (10-100+ nodes) | Advanced Node-to-Node DSP/Diagnostics |

---

## 6. Commercial Ecosystem Alignment (Hearing Aids & Android 17)

Commercial Bluetooth LE Audio ecosystems operate under strict roles:
1. **Auracast Hearing Aids & Earbuds**:
   - Implement **BASS (`0x184F`)** and **VCS (`0x1844`)** as GATT Servers.
   - Rely on a smartphone (Android 15+ / iOS 18+) or remote assistant to write to BASS with the target `Broadcast_ID`.
2. **Smart Speakers & Public Transmitters**:
   - Transmit continuous BIS audio streams while listening for BASS client requests.
3. **Firmware Strategy for forestChirp / ble_audio**:
   - Implementing **BASS (`0x184F`)** directly on the ESP32-C6 SINK firmware ensures that the node can be controlled by our custom ESP32-C6 SOURCE today, and seamlessly controlled by **Android 17 / Pixel devices** as a standard Auracast audio receiver tomorrow.
