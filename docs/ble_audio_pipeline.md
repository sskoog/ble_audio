
# 1. Auracast and BLE Audio Architecture

Auracast is a major paradigm shift in wireless audio transmission, introduced by the Bluetooth SIG as part of the **Bluetooth Low Energy (LE) Audio** specification.

Whereas Classic Bluetooth (BR/EDR) uses a point-to-point connection scheme (like a private phone call over A2DP), Auracast is a **connectionless broadcast protocol** (analogous to an FM radio station broadcasting to an unlimited number of receivers within range).

```
+------------------+         Broadcast Isochronous Stream (BIS)
| Auracast Source  | -------------------------------------------------> [ Speaker 1 Receiver ]
| (e.g. QCC3086 /  | -------------------------------------------------> [ Speaker 2 Receiver ]
|  QCC5181 Hub)    | -------------------------------------------------> [ Speaker 3 Receiver ]
+------------------+         (Unlimited Passive Receivers)
```

## Core Architecture Components

- **Isochronous Channels (ISOC):** Introduced in Bluetooth Core Specification 5.2, ISOC provides time-bounded data delivery mechanism at the Link Layer for time-sensitive data such as audio.
- **Broadcast Isochronous Streams (BIS):** The logical transport mechanism used by the broadaster to transmit one mono audio channel, or an interleaved stereo channel unidirectionally without individual receiver handshakes or acknowledgments.
- **Broadcast Isochronous Group (BIG):** A collection of one or more synchronized BIS. The default setup when usign BIGs is sending mono audio in every BIS, e.g. a 5.1 channel audio can be broadcasted in 6 BIS. The receiver can then select which BIS to listen to, and ignore the irrelevant ones to save power.
- **Public Broadcast Profile (PBP):** Defines the standard profile requirements for broadcasting and receiving public or private audio streams via Auracast, ensuring cross-vendor interoperability.
- **Broadcast Audio Scan Service (BASS):** A GATT-based service that enables an assistant device (like a smartphone app) to scan for broadcast sources and instruct a target receiver (like a wireless speaker or earbud) to sync to a specific BIG stream.

## Time Synchronization and Latency Mechanics

Multi-speaker wireless networks natively suffer from phase cancellation, comb filtering, and echo if receivers fall out of sync by even a few milliseconds. Auracast solves this natively at the hardware protocol level:

- **Microsecond Inter-Receiver Sync:** All receivers locked onto a BIG derive timing from a common deterministic Link Layer clock embedded in the broadcast header. Independent receivers maintain inter-speaker synchronization within **5 to 10 microseconds (µs)**.
- **Ultra-Low Latency:** End-to-end transport latency for LE Audio with LC3 is typically **20 ms to 45 ms** (compared to 150 ms–300 ms on Classic Bluetooth SBC/AAC).
- **Presentation Delay Buffer:** The transmitter includes a `Presentation Delay` parameter in the metadata. Receivers buffer incoming audio frames and trigger exact digital-to-analog conversion at the exact microsecond timestamp prescribed by the broadcaster, overcoming local jitter and processing variance.

## Bluetooth Version & Compatibility Requirements

| Bluetooth Core Spec | LE Audio ISOC Support | Mandatory Auracast Support? | Notes |
| :--- | :--- | :--- | :--- |
| **Bluetooth 5.0 / 5.1** | No | No | Classic Bluetooth (BR/EDR) only. Incompatible with Auracast. |
| **Bluetooth 5.2** | Primitive (ISOC introduced) | No | Optional feature. Most BT 5.2 chips only support LE Data or Unicast LE Audio. |
| **Bluetooth 5.3** | Full Support | No (Optional Profile) | Hardware supports BIS/BIG. Auracast requires PBP profile implementation. |
| **Bluetooth 5.4** | Full Support + Enhancements | No (Optional Profile) | Improved periodic advertising with responses (PAwR) and enhanced encryption. |

> [!WARNING]
> A product or DIY module labeled "Bluetooth 5.3" or "Bluetooth 5.4" does **NOT** automatically support Auracast. If the datasheet does not explicitly list **LE Audio**, **BIS (Broadcast Isochronous Streams)**, **PBP (Public Broadcast Profile)**, or **Auracast**, assume it is a standard Classic Bluetooth (A2DP) device.

---

## Learnings
* Audio CAN be sent via Periodic Advertising (PA), however, the collission rate is high, and there is no support for retransmission in this mode. Packets will most likely collide with 2.4 GHz WiFi and other Bluetooth devices. Use this only as a fallback plan for audio transmission! 
* ...


---



---

# 2. Espressif ESP32-C6 SoC: LE Audio Reality & Controller Implementation Findings

During hands-on implementation and reverse-engineering of the Bluetooth 5.3 LE Audio stack on the ESP32-C6 (ESP-IDF v6.0.2 with NimBLE), critical architectural findings were uncovered regarding what is *theoretically in the Bluetooth 5.3 spec* versus what is *actually implemented in Espressif's precompiled controller firmware blob* (`libble_app.a`).

```
+───────────────────────────────────────────────────────────────────────────────────────────+
|                      ESP32-C6 CONTROLLER (libble_app.a) CAPABILITY MATRIX                |
+───────────────────────────────────────────────────────────────────────────────────────────+
| Feature / HCI Command                     | Opcode (OCF) | Supported in HW? | Actual Result |
|───────────────────────────────────────────|──────────────|──────────────────|───────────────|
| Extended Advertising (LE 1M / LE 2M)      | 0x0036       | YES              | SUCCESS       |
| Periodic Advertising Train (PA TX)        | 0x003E       | YES              | SUCCESS       |
| Periodic Advertising Sync (PA RX)         | 0x0044       | YES              | SUCCESS       |
| BIG Broadcaster (HCI_LE_Create_BIG)       | 0x0068       | YES              | SUCCESS       |
| BIG Receiver (HCI_LE_BIG_Create_Sync)     | 0x006B       | NO (Missing LL)  | ERR 0x201 (Unknown Cmd) |
| Read Buffer Size v2 (LE_RD_BUF_SIZE_V2)   | 0x0060       | NO (V1 only)     | ERR 0x201 (Unknown Cmd) |
| ISO Host TX (ble_hci_trans_hs_iso_tx)     | N/A          | NO               | Linker Error  |
+───────────────────────────────────────────────────────────────────────────────────────────+
```

# 2.2: Further notes on ESP32-family support for BLE ISOC audio:
Espressif Systems (ESP32-C6, ESP32-H4, ESP32-S31): Espressif utilizes ESP-IDF with the open-source Apache NimBLE stack. While the ESP32-C6 (BT 5.3) natively supports Isochronous channels, Espressif’s recently announced ESP32-H4 and ESP32-S31 (both BT 5.4) are specifically targeted at LE Audio, featuring dedicated DSP extensions for LC3 vector processing and extensive PSRAM support for audio buffering.


---

## 2.1: Key Architectural Findings & Root Causes

### 1. `HCI_LE_BIG_Create_Sync` Rejection (`0x201 BLE_ERR_UNKNOWN_HCI_CMD`)
- **The Issue**: When the SINK node discovers an Auracast stream and calls `ble_gap_big_create_sync()` to lock onto the Broadcast Isochronous Group (BIG), the ESP32-C6 controller firmware immediately returns:
  ```
  NimBLE: ogf=0x08, ocf=0x006b, hci_err=0x201 : BLE_ERR_UNKNOWN_HCI_CMD
  BLE_AUDIO: SINK: Called ble_gap_big_create_sync() for BIS #1 (rc = 513)
  ```
- **Root Cause in Binary Blob**: Binary symbol analysis using `riscv32-esp-elf-nm` on `esp-idf/components/bt/controller/lib_esp32c6/esp32c6-bt-lib/esp32c6/libble_app.a` reveals:
  ```
  ble_ll_iso.c.o: no symbols
  ```
  The BIG Synchronized Receiver Link Layer state machine is **completely absent from the ESP32-C6 controller binary library** in current ESP-IDF releases (v5.x through v6.0).
- **Asymmetry**: The ESP32-C6 controller **does** support `HCI_LE_Create_BIG` (Broadcaster / Source role), but **cannot** act as a hardware `BIG_Create_Sync` Receiver (Sink role).

---

### 2. `HCI_LE_Read_Buffer_Size_v2` Failure on Stack Boot
- **The Issue**: Enabling `CONFIG_BT_NIMBLE_ISO=y` causes the NimBLE host stack to issue `HCI_LE_Read_Buffer_Size_v2` (`OGF 0x08, OCF 0x0060`) during startup to query ISO buffer depth.
- **Controller Response**: The ESP32-C6 controller returns `0x201 BLE_ERR_UNKNOWN_HCI_CMD` because it only implements v1 (`HCI_LE_Read_Buffer_Size` `0x08, 0x001B`).
- **Fix**: The host stack (`ble_hs_startup.c`) must implement an automatic fallback from v2 to v1, assigning default ISO buffer sizes (251 bytes, 8 buffers) upon receiving `0x201`.

---

### 3. Missing ISO Transport Function in Host (`ble_hci_trans_hs_iso_tx`)
- **The Issue**: NimBLE's `ble_hs_iso.c` contains legacy references to `ble_hci_trans_hs_iso_tx()` which was removed in ESP-IDF's transport refactor.
- **Fix**: A forward declaration and stub must be supplied to satisfy the linker when compiling the ISO host stack for receiver applications.

---

## 2.2: Practical Implications for DIY Wireless Multi-Speaker Systems

Because the ESP32-C6 cannot execute hardware `BIG_Create_Sync`, developers building wireless multi-speaker networks on ESP32-C6 must choose between two working transport architectures:

```
                                  ESP32-C6 AUDIO TRANSPORT OPTIONS
                                                  │
                 ┌────────────────────────────────┴────────────────────────────────┐
                 ▼                                                                 ▼
      Option A: Periodic Advertising (PA)                             Option B: Connected Multi-Cast
        with Sliding-Window Redundancy                                  or Dedicated Auracast SoCs
                 │                                                                 │
  * Connectionless broadcast to unlimited sinks.                   * Dedicated Nordic nRF5340 / nRF54
  * RTN = 0 on physical radio.                                       or Qualcomm QCC5181 / QCC3084.
  * Solved via 3-frame sliding history buffer:                     * Full native BIG/BIS hardware sync.
    Packet K = [Frame N-2, N-1, N].                                * Sub-10 µs Link-Layer clock sync.
  * 0% audio dropouts, PLC drops from 40/s -> 0/s.                 * Hardware frequency-hopping (RTN=2-4).
```

### Why Raw Periodic Advertising (PA) Had 40 PLC/s Dropouts
1. Standard Periodic Advertising sends each packet once with **zero retransmissions ($RTN = 0$)**.
2. On the congested 2.4 GHz ISM band, Wi-Fi beacons and BLE channels cause ~35–40% packet collision.
3. Every lost 20 ms packet lost 2 audio frames, forcing the SINK's LC3 Packet Loss Concealment (PLC) engine to synthesize 40 frames/second.

### The Zero-Loss Solution: 3-Frame Sliding-Window Redundancy
By packing three consecutive LC3 frames ($N-2, N-1, N$) into each 20 ms PA packet (124 bytes total, well within the 251-byte MTU):
- Even if Packet $K$ is completely dropped over the air, Packet $K+1$ delivers the missing frames into the SINK's 40 ms jitter cushion before the I2S DMA plays them.
- Audio frame loss drops to **0.0%**, reducing `PLC` and `FIFO_UD` from **40/s to 0/s**.

---

## 2.3: Hardware Comparison: Native Auracast BIG Receiver Support

| SoC / Module | Vendor | BIG Broadcaster (TX) | BIG Receiver (RX / Sync) | Software Stack | Sinking Recommendation |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Nordic nRF5340** | Nordic Semi | **YES (Native)** | **YES (Native)** | Zephyr / nRF Connect SDK | **Best Open-Source Auracast SINK** |
| **Nordic nRF54L15 / H20**| Nordic Semi | **YES (Native)** | **YES (Native)** | Zephyr / nRF Connect SDK | **Next-Gen Open-Source SINK** |
| **Qualcomm QCC5181** | Qualcomm | **YES (Native)** | **YES (Native)** | Qualcomm ADK / AT Commands | **Best Commercial Audiophile SINK** |
| **Qualcomm QCC3084** | Qualcomm | **YES (Native)** | **YES (Native)** | Qualcomm ADK / AT Commands | **Best Budget SMT SINK Module** |
| **Espressif ESP32-C6** | Espressif | **YES (Bumble/TX)** | **NO (Missing in Blob)** | ESP-IDF (NimBLE) | **Use Redundant PA or Upgrade to H4** |
| **Espressif ESP32-H4** | Espressif | **YES (BT 5.4)** | **YES (Expected v5.4)** | ESP-IDF (Sampling Phase) | **Upcoming Espressif BT 5.4 Flagship** |

## 1.1: Auracast & BLE Audio Protocol Architecture

Bluetooth LE Audio introduces a layered architecture combining connectionless Link-Layer isochronous streams with Periodic Advertising and GATT control services.

```
+----------------------------------------------------------------------------------+
|                      Application / Profiles Layer                                |
|    Public Broadcast Profile (PBP)  |  Basic Audio Profile (BAP)                  |
+----------------------------------------------------------------------------------+
|                      Control Services Layer (GATT)                               |
|  BASS (Scan) | PACS (Capabilities) | VCS (Volume) | VOCS (Offset) | AICS (Input) |
+----------------------------------------------------------------------------------+
|                      Transport & Advertising Layer                               |
|   Isochronous Adaptation Layer (ISOAL)  |  Periodic Advertising (PA / PAwR)      |
+----------------------------------------------------------------------------------+
|                      Link Layer Architecture                                     |
|   Broadcast Isochronous Group (BIG) --> Broadcast Isochronous Streams (BIS)      |
+----------------------------------------------------------------------------------+
```

## 1.2: Isochronous Channels (ISOC) Architecture: Connected (CIS/CIG) vs. Broadcast (BIS/BIG)

Bluetooth Core Specification 5.2+ divides **Isochronous Channels (ISOC)** into two distinct Link-Layer transport paradigms: **Connected ISOC** (for paired point-to-point and multi-point audio) and **Broadcast ISOC** (for connectionless Auracast broadcasting).

```
+-----------------------------------------------------------------------------------------+
|                          BLUETOOTH LE AUDIO ISOCHRONOUS MODES                           |
+-----------------------------------------------------------------------------------------+
                                             |
        +------------------------------------+------------------------------------+
        |                                                                         |
        v                                                                         v
+-----------------------------------------+     +-----------------------------------------+
|   1. CONNECTED ISOC (CIS / CIG)         |     |   2. BROADCAST ISOC (BIS / BIG)         |
|   (Isochronous without BIGs!)           |     |   (Auracast Broadcast Mode)             |
+-----------------------------------------+     +-----------------------------------------+
| * Group Container: CIG                  |     | * Group Container: BIG                  |
| * Logical Streams: CIS (1 to 31)        |     | * Logical Streams: BIS (1 to 31)        |
| * Topology: Point-to-Point / Multipoint |     | * Topology: 1-to-Unlimited Broadcast    |
| * Transport: Paired BLE ACL Link        |     | * Transport: Periodic Train (BigInfo)   |
| * Packet Feedback: Hardware ACK / NACK  |     | * Packet Feedback: NONE (Passive RX)    |
| * Retransmission: Full ARQ Subevents    |     | * Retransmission: Blind Burst (RTN=2-4) |
| * Target: TWS Earbuds, 2.0 Stereo Pair  |     | * Target: Multi-Speaker, Public Audio   |
| * Node Limit: 2 to 4 Active Connections |     | * Node Limit: UNLIMITED Passive Sinks   |
+-----------------------------------------+     +-----------------------------------------+
```

### 1. Connected Isochronous Channels: CIS and CIG (ISOC Without BIGs)
* **Connected Isochronous Stream (CIS):** A point-to-point, bidirectional (or unidirectional) time-bounded logical stream established between two connected BLE devices (e.g., smartphone to Left Earbud, or Central Hub to Speaker A).
* **Connected Isochronous Group (CIG):** A container that groups between **1 and 31 individual CIS streams** that share a common time-base (e.g. `CIS 1` = Left Earbud, `CIS 2` = Right Earbud).
* **Hardware ACK / NACK Retransmissions (ARQ):** Because CIS operates over connected ACL links, the receiver actively acknowledges every packet. If a packet is lost or corrupted by 2.4 GHz Wi-Fi interference, the controller automatically retransmits it during subevents within the configured ISO interval, delivering near-zero packet dropouts.
* **Microsecond Inter-Stream Synchronization:** Left and Right CIS streams within a CIG maintain sub-10 µs inter-channel phase alignment.
* **Hardware & Scalability Ceiling:** A Central radio can typically maintain only **2 to 4 concurrent CIS connections** due to radio scheduling slot limitations.

---

### 2. Broadcast Isochronous Channels: BIS and BIG (The Auracast Mode)
* **Broadcast Isochronous Stream (BIS):** An unacknowledged, unidirectional logical stream carrying LC3 compressed audio payload frames broadcast to an unlimited audience without handshakes or connections.
  * *Channel Content & Payload:* A single BIS payload can carry a mono channel or an interleaved stereo pair (L+R). Under standard BAP/PBP Auracast, the standard configuration is **1 Mono audio channel per BIS stream** (e.g. `BIS 1` = Left, `BIS 2` = Right), allowing individual satellite speakers or earbuds to listen only to their assigned stream to conserve power.
* **Broadcast Isochronous Group (BIG):** A group container synchronization wrapper that groups between **1 and 31 individual BIS streams** sharing the exact same deterministic Link-Layer clock time-base.
* **Retransmissions (RTN):** Receivers are purely passive and cannot transmit ACK/NACK responses back to the broadcaster. The broadcaster relies on blind burst retransmissions (**RTN = 2 to 4**) across frequency-hopped channels.

---

### Comparison: Connected ISOC (CIS/CIG) vs. Broadcast ISOC (BIS/BIG)

| Architectural Metric | Connected ISOC (CIS / CIG) | Broadcast ISOC (BIS / BIG / Auracast) |
| :--- | :--- | :--- |
| **Requires BIG?** | **NO** (Uses CIG container) | **YES** (Mandatory BIG container) |
| **Connection Required?** | **YES** (Standard BLE GAP / ACL link) | **NO** (100% Connectionless) |
| **Max Receivers** | **2 to 4 nodes** (TWS Earbuds, 2.0 Stereo Pair) | **UNLIMITED** (Public Broadcast / Multi-Speaker) |
| **Packet Acknowledgment** | **YES (Bidirectional ACK / NACK ARQ)** | **NO** (Passive Unidirectional) |
| **RF Collision Immunity** | **Ultra-High** (Automatic packet retry) | **Medium** (Blind RTN retransmissions) |
| **ESP32-C6 Controller Support** | **YES** (`CONFIG_BT_NIMBLE_ISO_CIS/CIG`) | **Broadcaster Only** (SINK Sync missing in blob) |

## 1.3: Advertising Layer: Extended & Periodic Advertising (PA / PAwR)
Connectionless broadcast discovery operates via two distinct advertising mechanisms:
- **Extended Advertising (EA):** Broadcasts initial presence beacon on primary advertising channels (37, 38, 39) pointing receivers to secondary advertising channels.
- **Periodic Advertising (PA):** Transmits synchronized metadata trains at fixed intervals (every 7.5 ms to 4s). Contains **BIGInfo** (which defines encryption keys, sample rates, frame durations, and BIS transport schedules).
- **PAwR (Periodic Advertising with Responses - BT 5.4):** Allows receivers to send small control responses back during designated subevents without establishing full ACL connections.

## 1.4: GATT Control Services & Pre-Defined LE Audio Metadata Fields
Even in broadcast mode, control and configuration rely on standardized GATT services and LTV (Length-Type-Value) metadata structures.

| Service / Profile | Characteristic / Data Field | UUID / Type | Intended Data Direction | Pre-Defined Data Format | Practical Role in Multi-Speaker Setup |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Volume Offset (VOCS)** | **Audio Location** | `0x2B81` | **App/Hub $\leftrightarrow$ Sink** | 32-bit Bitfield | Assigns exact physical role (Bit 0: Front L, Bit 3: Sub 1, Bit 12: Top Front L, etc.) |
| **Volume Offset (VOCS)** | **Volume Offset State** | `0x2B80` | **Sink $\rightarrow$ App/Hub** (Notify) | Int16 (0.01 dB) | Reports individual speaker gain trim offset (-12.00 dB to +12.00 dB) |
| **Volume Offset (VOCS)** | **Audio Output Description**| `0x2B83` | **App/Hub $\leftrightarrow$ Sink** | UTF-8 String | Sets/reports human-readable label (e.g. "Ceiling Left Front", "Subwoofer 1") |
| **Volume Control (VCS)** | **Volume State** | `0x2B7D` | **Sink $\rightarrow$ App/Hub** (Notify) | Uint8 + Bool | Reports current master volume (0–255), mute state, & `Change Counter` |
| **Volume Control (VCS)** | **Volume Control Point**| `0x2B7E` | **App/Hub $\rightarrow$ Sink** (Write) | Opcode + Counter | Executes volume down, volume up, absolute set, mute, and unmute commands |
| **Media Control (MCS)** | **Media Control Point** | `0x2BA4` | **App/Client $\rightarrow$ Player** (Write) | Opcode Uint8 | Stream transport controls (Play, Pause, Fast Forward, Next, Stop) |
| **Media Control (MCS)** | **Track Title / Player Name** | `0x2B96` / `0x2B93` | **Player $\rightarrow$ App/Sink** (Notify) | UTF-8 String | Broadcasts song title or active DSP EQ Preset Name (e.g. "Movie Surround") |
| **Capabilities (PACS)** | **Sink Audio Locations** | `0x2B9D` | **Sink $\rightarrow$ Broadcaster** (Read)| 32-bit Bitfield | Sink advertises which physical speaker locations its hardware supports |
| **Capabilities (PACS)** | **PAC Records** | `0x2B99` | **Sink $\rightarrow$ Broadcaster** (Read)| LTV Structures | Sink exposes supported LC3 sample rates, bit depths, and frame durations |
| **BAP / Auracast Metadata**| **Streaming Audio Context**| LTV `0x02` | **Broadcaster $\rightarrow$ Sinks** (PA) | 16-bit Bitmask | Broadcasts audio category (Bit 1: Music, Bit 2: Voice, Bit 3: Game, Bit 6: Emergency) |
| **BAP / Auracast Metadata**| **Language** | LTV `0x03` | **Broadcaster $\rightarrow$ Sinks** (PA) | 3-byte ISO 639-1 | Audio language identifier (e.g., "eng", "ger", "swe") |
| **BAP / Auracast Metadata**| **Vendor-Specific LTV** | LTV `0xFF` | **Broadcaster $\rightarrow$ Sinks** (PA) | Custom Bytes | Broadcasts custom PEQ biquad coefficients, crossover point (Hz), or delay (µs) |

## 1.5: Volume Control Service Architecture: `Volume State` vs. `Volume Control Point`

Bluetooth LE Audio separates volume management into a **Status Readout** (`Volume State`) and a **Command Inbox** (`Volume Control Point`) to prevent race conditions in multi-controller systems:

```
[ Smartphone App / Master Controller ]
        |
        +--- (Read / Subscribe Notify) ---> Volume State (0x2B7D) [Sink -> Client]
        |
        +--- (Write Opcode + Counter) ----> Volume Control Point (0x2B7E) [Client -> Sink]
```

1. **`Volume State` (`0x2B7D` - Read / Notify):**
   - **Direction:** **Audio Sink Device $\rightarrow$ Smartphone App / Controller**
   - **Data Payload (3 Bytes):**
     - `Volume Setting` (1 byte, uint8): Absolute volume level (`0` = silence to `255` = maximum volume).
     - `Mute` (1 byte, uint8): Mute status (`0` = Unmuted, `1` = Muted).
     - `Change Counter` (1 byte, uint8): Incrementing counter (`0..255`) that ticks up by +1 whenever volume or mute state changes.

2. **`Volume Control Point` (`0x2B7E` - Write / Write Without Response):**
   - **Direction:** **Smartphone App / Controller $\rightarrow$ Audio Sink Device**
   - **Opcode Command Table:**

| Opcode | Command Action | Direction | Required Payload Structure | Intention |
| :--- | :--- | :--- | :--- | :--- |
| **`0x00`** | **Relative Volume Down** | Client $\rightarrow$ Sink | `[0x00, Change Counter]` | Decrements sink volume by 1 step |
| **`0x01`** | **Relative Volume Up** | Client $\rightarrow$ Sink | `[0x01, Change Counter]` | Increments sink volume by 1 step |
| **`0x02`** | **Unmute** | Client $\rightarrow$ Sink | `[0x02, Change Counter]` | Restores audio playback |
| **`0x03`** | **Mute** | Client $\rightarrow$ Sink | `[0x03, Change Counter]` | Mutes audio playback |
| **`0x04`** | **Set Absolute Volume** | Client $\rightarrow$ Sink | `[0x04, Change Counter, Target Vol (0..255)]` | Sets exact master volume level |
| **`0x05`** | **Unmute & Set Absolute** | Client $\rightarrow$ Sink | `[0x05, Change Counter, Target Vol]` | Unmutes and sets absolute volume |
| **`0x06`** | **Mute & Set Absolute** | Client $\rightarrow$ Sink | `[0x06, Change Counter, Target Vol]` | Mutes and sets absolute volume |

3. **Intention of the `Change Counter` Guard:**
   - Every opcode command sent to `Volume Control Point` **must include the sink's latest `Change Counter`**.
   - If a client issues a command based on an outdated counter (e.g. another controller modified the volume in the millisecond before), the sink **rejects the write** with error code `0x80 (Invalid Change Counter)`.
   - This ensures atomic synchronization across multiple control devices (e.g. app + physical knob) without overwriting out-of-order changes.

## 1.6: Bluetooth Audio Codecs

Bluetooth audio relies on lossy and lossless audio codecs. Auracast specifically mandates the **LC3** codec for universal broadcast interoperability.

### Codec Comparison Table
Assuming both mono and stereo audio streams.

| Codec Name | Developer / Standard | Architecture | Bitrate (kbps) | Max Sample Rate (kHz) | Max Bit Depth (bits) | Latency (ms) | Auracast Broadcast Relevant? | DIY Open-Source Firmware? |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **LC3** | Bluetooth SIG / Fraunhofer IIS | LE Audio | 16 – 345 | 48 | 32 | 20–30 | **MANDATORY** (Core Auracast Standard) | **YES** (Standard in Zephyr/ESP-IDF) |
| **LC3plus** | ETSI (TS 103 634) / Fraunhofer | LE Audio / Classic | 16 – 500+ | 96 | 24 | 5–10 | **Optional** (Vendor-specific LE Extension) | **NO** (Commercial License Required) |
| **aptX Lite** | Qualcomm | LE Audio | 96 – 192 | 48 | 16 | ~20 | **Optional** (Qualcomm LE Broadcast) | **NO** (Proprietary / Closed Source) |
| **aptX** | Qualcomm | Classic (A2DP) | 176 – 384 | 44.1 | 16 | 120–150 | **Incompatible** (Classic Unicast Only) | **NO** (Proprietary / Closed Source) |
| **aptX LL** | Qualcomm | Classic (A2DP) | 176 – 384 | 44.1 | 16 | <40 | **Incompatible** (Deprecated Classic) | **NO** (Proprietary / Closed Source) |
| **aptX HD** | Qualcomm | Classic (A2DP) | 288 – 576 | 48 | 24 | 150–200 | **Incompatible** (Classic Unicast Only) | **NO** (Proprietary / Closed Source) |
| **aptX Adaptive** | Qualcomm | Classic (A2DP) | 279 – 860 | 96 | 24 | 50–80 | **Incompatible** (Classic Unicast Only) | **NO** (Proprietary / Closed Source) |
| **aptX Lossless** | Qualcomm (Snapdragon Sound) | Classic (A2DP) | 140 – 1200 | 44.1 | 16 | 80–100 | **Incompatible** (Classic Unicast Only) | **NO** (Proprietary / Closed Source) |
| **SBC** | Bluetooth SIG | Classic (A2DP) | 127 – 345 | 48 | 16 | 100–250 | **Incompatible** (Classic Unicast Only) | **YES** (Universal A2DP Standard) |
| **AAC** | Dolby / MPEG | Classic (A2DP) | 64 – 320 | 44.1 | 16 | 120–200 | **Incompatible** (Classic Unicast Only) | **YES** (via FDK-AAC port) |
| **LDAC** | Sony | Classic (A2DP) | 330 – 990 | 96 | 24 | 150–200 | **Incompatible** (Classic Unicast Only) | **Encoder Only** (Decoder Proprietary) |
| **LHDC / LLAC** | Savitech | Classic (A2DP) | 128 – 900 | 96 | 24 | 30–80 | **Incompatible** (Classic Unicast Only) | **NO** (Proprietary / Closed Source) |

### Key Codec Insights for Auracast DIY Projects

1. **LC3 (Low Complexity Communication Codec):**
   - The mandatory standard for Bluetooth LE Audio and Auracast.
   - Provides superior subjective audio quality at **half the bitrate** of legacy SBC (e.g., 160 kbps LC3 outperforms 328 kbps SBC).

2. **LC3plus (High-Resolution LE Audio Extension):**
   - Developed by Fraunhofer IIS and standardized under ETSI TS 103 634.
   - Operates as an officially supported, vendor-specific high-resolution extension for BLE Audio Broadcast Isochronous Streams (BIS).
   - Capable of fulfilling "Hi-Res Audio Wireless" certification, governed by the Japan Audio Society (JAS): >=96 kHz @ 24-bit.
   - Supports extended bit depths (up to 32-bit), higher sampling rates (up to 96 kHz), and lower latency (as low as 5 ms) compared to the base LC3 standard.
   - Ideal for specialized, high-fidelity wireless broadcast networks (e.g., gaming headsets or high-end PA systems) where the baseline LE Audio capabilities are insufficient, provided that both the broadcaster and receivers support it.
   - Supported natively by modern operating systems (Android 13+, Windows 11 22H2+, iOS 18+).
3. **Classic High-Resolution Audio Codecs (LDAC, aptX HD, aptX Lossless):**
   - Do Not Work in Auracast!
   - LDAC, aptX HD, and aptX Lossless rely on Classic A2DP point-to-point packet acknowledgment and retransmission protocols (ARQ).
   - Broadcast Isochronous Streams (BIS) are connectionless; receivers cannot transmit acknowledgment (ACK/NACK) packets back to the source. Therefore, Auracast networks strictly execute using LC3 or specialized LE broadcast frame structures.

<!-- <img src="./images/codec_table.png" width=400> -->



---

# 3. Wireless Audio Architectures for 1-to-6 SINKs: Comprehensive Engineering Comparison

When engineering a wireless multi-speaker network (1 SOURCE streaming real-time, high-fidelity audio to 1–6 independent SINK speaker nodes), maintaining **microsecond-accurate inter-node time synchronization** and **zero audible packet loss** are the two critical constraints.

Without microsecond synchronization, phase cancellation and comb filtering severely degrade acoustic quality. Without robust packet loss mitigation, 2.4 GHz RF collisions with local Wi-Fi and Bluetooth devices cause audible clicks, pops, or dropouts.

Below is an in-depth technical analysis and side-by-side comparison of the six primary wireless architectures available today:

```
+───────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
|                                    WIRELESS MULTI-SPEAKER TRANSPORT ARCHITECTURES                                 |
+───────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
        │
        ├── [1] BLE 5.3 Audio BIG/BIS (Auracast) ─────────────── Link-Layer Broadcast ISOC (Unlimited Sinks)
        │
        ├── [2] BLE 5.3 Audio CIG/CIS ────────────────────────── Link-Layer Connected ISOC with ARQ (1-4 Sinks)
        │
        ├── [3] BLE Audio via Periodic Advertising (PA) ──────── Connectionless BLE Beacon with 3-Frame Redundancy
        │
        ├── [4] BLE Audio via Multi-GATT & 2M PHY ────────────── Asynchronous ACL Links with PTP Presentation Delay
        │
        ├── [5A] Wi-Fi via ESP-NOW (Raw PCM) ────────────────── Low-overhead 802.11 Action Frames (Uncompressed)
        │
        ├── [5B] Wi-Fi via ESP-NOW + LC3 Codec ──────────────── High-Efficiency 120-byte Packets with 2x Redundancy
        │
        └── [6] Wi-Fi via Existing Router / AP ───────────────── Standard UDP/RTP Multicast with PTP IEEE 1588
```

---

## 3.1: Master Technical Comparison Matrix

| Architectural Feature | [1] BLE 5.3 BIG/BIS | [2] BLE 5.3 CIG/CIS | [3] BLE PA + Redundancy | [4] BLE Multi-GATT (2M) | [5A] ESP-NOW (Raw PCM) | [5B] ESP-NOW + LC3 | [6] Wi-Fi AP (PTP) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Topology** | 1-to-Unlimited | 1-to-4 Paired | 1-to-Unlimited | 1-to-6 Paired | 1-to-20 Peer | **1-to-Unlimited** | 1-to-N Multicast |
| **Inter-Node Time Sync** | **< 10 µs** | **< 10 µs** | ~100–200 µs | ~30–50 µs | **< 5 µs** | **< 5 µs** (HW MAC TS) | < 10 µs (PTP) |
| **Packet Loss Protection** | Blind Retransmit | **Hardware ARQ** | **3-Frame Window** | **Hardware ARQ** | HW ACK (Unicast) | **2x Frame Cloning ($N-1, N$)**| TCP / RTP Buffer |
| **RF Collision Immunity** | Medium | **Ultra-High (0%)** | **Ultra-High (0%)** | **Ultra-High (0%)** | Medium (Broadcast) | **Ultra-High (0% Loss)** | Low to High |
| **Transport Latency** | 20–30 ms | 15–25 ms | 40–60 ms | 30–50 ms | **5–15 ms** | **10–15 ms** | 50–200 ms |
| **Audio Format / Quality**| 48 kHz / 24b LC3 | 48 kHz / 24b LC3 | 48 kHz / 16b LC3 | 48 kHz / 24b LC3 | 48/96 kHz Raw PCM | **48 kHz / 24b LC3 (HQ)** | 48/192 kHz PCM |
| **Packet Size & Fragmentation**| 120B (No frag) | 120B (No frag) | 124B (No frag) | 120B (No frag) | 960B (**4 fragments**) | **120B / 240B (No frag)** | MTU 1460B |
| **Packets / sec (4 Sinks)**| 400 pkts/s | 400 pkts/s | 200 pkts/s | 400 pkts/s | 1,600 pkts/s | **400 pkts/s (4x fewer)**| 400 pkts/s |
| **Radio Airtime (4 Sinks)**| ~25% (2M PHY) | ~30% (2M PHY) | ~45% (2M PHY) | ~33% (2M PHY) | ~20% (802.11g) | **~2.8% (97% Free Air)** | ~5% (802.11ax) |
| **Power Draw (SOURCE)** | **~15 mA** | **~18 mA** | **~15 mA** | **~20 mA** | ~95–110 mA | **~35–45 mA (~60% drop)**| ~110–160 mA |
| **Power Draw (SINK)** | **~15 mA** | **~18 mA** | **~15 mA** | **~20 mA** | ~85–90 mA | **~30–38 mA (Modem-Sleep)**| ~100–140 mA |
| **Standalone Operation?** | **YES** | **YES** | **YES** | **YES** | **YES** | **YES** | **NO** (Needs AP) |
| **ESP32-C6 Readiness** | TX only | Host only | **100% Working** | **100% Working** | **100% Working** | **100% Working** | **100% Working** |

---

## 3.2: Architectural Deep-Dive

### 1. BLE 5.3 Audio BIG / BIS (Auracast Broadcast)
* **How It Works**: Source broadcasts a Broadcast Isochronous Group (BIG) containing 1 to 31 Broadcast Isochronous Streams (BIS). Receivers listen passively without pairing or connecting.
* **Time Synchronization**: **Sub-10 µs**. All receivers lock their Link-Layer hardware clock to the deterministic periodic anchor points of the BIG.
* **Reliability & Packet Loss**: Sinks cannot transmit ACKs. The transmitter broadcasts each packet multiple times ($RTN = 2	ext{ to }4$) on pseudo-random frequency hops (PTO). In heavily congested 2.4 GHz environments, sporadic frame drops can occur, which the LC3 PLC engine conceals.
* **Limitations**: Current ESP32-C6 / ESP32-H2 controller firmware blobs (`libble_app.a`) reject `HCI_LE_BIG_Create_Sync`. Requires dedicated Auracast silicon (Nordic nRF5340/nRF54, Qualcomm QCC5181).

---

### 2. BLE 5.3 Audio CIG / CIS (Connected Isochronous Streams)
* **How It Works**: Source establishes standard point-to-point BLE connections with 1 to 4 SINKs and binds them into a Connected Isochronous Group (CIG) with discrete CIS streams.
* **Time Synchronization**: **Sub-10 µs**. All CIS streams share the CIG time-base anchor points.
* **Reliability & Packet Loss**: **0.0% loss**. Features full **bidirectional hardware ACK/NACK (ARQ)**. If a packet collides with Wi-Fi, the radio retransmits it immediately in a scheduled subevent within the 10 ms interval.
* **Limitations**: Maximum 2 to 4 SINK nodes due to radio scheduling slot ceilings. (ESP32-C6 controller blob currently lacks CIS Link Layer).

---

### 3. BLE Audio via Periodic Advertising (PA) with 3-Frame Redundancy
* **How It Works**: Source broadcasts audio frames inside standard Bluetooth 5.0 Periodic Advertising packets (`AUX_SYNC_IND`) at 50 Hz (20 ms cadence).
* **Time Synchronization**: **~100–200 µs**. Sinks synchronize local clocks to the Periodic Advertising train anchor point.
* **Reliability & Packet Loss**: Raw PA has a 35–40% RF collision rate ($RTN = 0$). However, by packing a **3-frame sliding history window** ($[N-2, N-1, N]$) into each 20 ms burst, dropped packets are 100% recovered by the receiver's 40 ms jitter buffer, driving **`PLC` and `FIFO_UD` to 0**.
* **Advantages**: Runs out-of-the-box on 100% of ESP32-C6 and ESP32-H2 hardware today with unlimited passive sinks.

---

### 4. BLE Audio via Multi-GATT / L2CAP CoC on LE 2M PHY
* **How It Works**: Source acts as BLE Central and establishes paired ACL connections with 1 to 6 SINKs. Audio is transmitted via high-throughput L2CAP Connection-Oriented Channels or GATT Notifications over the **2.0 Mbps PHY**.
* **Time Synchronization**: **~30–50 µs**. Source embeds a microsecond Presentation Timestamp ($T_{	ext{play}} = T_{	ext{capture}} + 50	ext{ ms}$) in every frame. Sinks maintain clock synchronization via periodic round-trip PTP pings and release audio samples to the I2S DMA at the exact target timestamp.
* **Reliability & Packet Loss**: **0.0% loss**. Standard BLE ACL connections provide full hardware ARQ retransmissions. Because LE 2M PHY transmits each 120-byte frame in just $\sim 564	ext{ }\mu	ext{s}$, 4 streams consume only **32.8% of airtime**, leaving **67.2% free margin** for instant retries.
* **Advantages**: Fully supported across all ESP32-C6, ESP32-S3, and ESP32 nodes today.

---

### 5A. Wi-Fi via ESP-NOW (Raw Uncompressed PCM)
* **How It Works**: Uses Espressif's connectionless Wi-Fi protocol (ESP-NOW), transmitting raw 802.11 vendor-specific action frames directly between ESP32 chips without an access point.
* **Time Synchronization**: **< 5 µs** via hardware MAC-layer packet arrival timestamps.
* **The Fragmentation Bottleneck**: ESP-NOW has a **250-byte maximum packet payload**. Raw 48 kHz 16-bit mono PCM produces 960 bytes every 10 ms, forcing the transmitter to fragment each audio frame into **4 separate packets** (1,600 packets/s for 4 SINKs), keeping the Wi-Fi radio active ~20% of the time and drawing ~95–110 mA.

---

### 5B. Wi-Fi via ESP-NOW with LC3 Codec & Dual-Frame Redundancy (High-Efficiency Tier)
* **How It Works**: Combines Google `liblc3` compression with ESP-NOW connectionless action frames. 48 kHz mono audio is compressed to **120 bytes per 10 ms frame** (96 kbps studio tier).
* **Zero Packet Fragmentation**: A 120-byte LC3 frame fits inside a single 250-byte ESP-NOW packet without fragmentation. Packet throughput drops by **4x** (from 1,600 pkts/s to **400 pkts/s** for 4 speakers).
* **60% Power Consumption Reduction**:
  - **SOURCE Current**: Drops from ~105 mA to **~35–45 mA**.
  - **SINK Current**: SINK only needs to listen for a ~70 µs burst every 10 ms and enters Wi-Fi **Modem-Sleep** during the remaining 9.3 ms, dropping current from ~90 mA to **~30–38 mA**.
  - **Battery Life**: Triples standard 2000 mAh runtime from **~20 hours to ~60+ hours**.
* **Zero-Loss Connectionless Broadcast via 2x Frame Cloning ($N-1, N$)**:
  - Because 120 bytes is only half of the 250-byte MTU, each packet carries **Frame $N-1$ and Frame $N$ (240 bytes total)**.
  - In connectionless broadcast mode (no receiver ACKs), if Packet $K$ collides with 2.4 GHz Wi-Fi interference, Packet $K+1$ delivers the missing frame into the 20 ms jitter buffer, delivering **100% loss-free audio with 0 PLC**.
* **Ultra-Low Airtime Congestion**: Active on-air RF duration drops to **only 2.8%** of each 10 ms interval, leaving 97.2% of 2.4 GHz airtime completely free for household Wi-Fi.

---

### 6. Wi-Fi via Existing Router / Access Point (UDP Multicast + PTP IEEE 1588)
* **How It Works**: All speaker nodes and the audio source connect to a standard home or venue Wi-Fi router (2.4 GHz / 5 GHz). Audio is broadcast via UDP/RTP Multicast or discrete UDP unicast streams, synchronized via **PTP (Precision Time Protocol / IEEE 1588 / AES67 / Dante / Apple AirPlay 2)**.
* **Time Synchronization**: **< 10 µs** using standard hardware-assisted PTP clock servo algorithms.
* **Reliability & Packet Loss**: 
  - Standard consumer Wi-Fi routers handle Multicast poorly (transmitting at the slowest legacy 802.11 rates without ACKs), causing 15–30% packet loss unless IGMP snooping / multicast-to-unicast is supported on enterprise-grade access points.
  - Requires large jitter buffers (**50–200 ms**) to absorb router queuing variance.
* **Tradeoff**: Not standalone (requires an external router); higher latency and power draw.

---

## 3.3: Recommended Roadmap for ForestChirp Multi-Speaker Audio

```
                                  RECOMMENDED DEVELOPMENT ROADMAP
                                                  │
                 ┌────────────────────────────────┴────────────────────────────────┐
                 ▼                                                                 ▼
      PHASE 1: IMMEDIATE ESP32-C6 DEPLOYMENT                       PHASE 2: DEDICATED AUDIOPHILE SILICON
      (Zero New Hardware Required)                                (Hardware Upgrade Path)
                 │                                                                 │
  * Mode A: BLE Multi-GATT on LE 2M PHY                            * Nordic nRF54L15 / nRF5340 or Qualcomm QCC5181
    - 1 to 4 SINKs with Hardware ARQ (0% loss).                      - Full native Auracast BIG/BIS Broadcast.
    - Timestamped Presentation Delay (< 50 µs sync).                 - Sub-10 µs Link-Layer hardware clock sync.
  * Mode B: Wi-Fi ESP-NOW                                          * Espressif ESP32-H4 (Bluetooth 5.4 Flagship)
    - Ultra-low latency (10 ms) & uncompressed PCM.                  - Full native ISOC hardware support.
```


# 4. Bluetooth 5.0+ Physical Layer (PHY) Modes in Auracast

In Bluetooth 5.0 and later, the Physical Layer (PHY) determines the digital symbol rate, modulation scheme, and forward error correction (FEC) applied to radio signals over the 2.4 GHz ISM band. Selecting the optimal PHY is critical for balancing multi-channel audio bandwidth, range, and packet collision immunity.

```
+----------------------------------------------------------------------------------------------------+
|                                    BLUETOOTH 5.x PHYSICAL LAYERS                                   |
|                                                                                                    |
|  1. LE 1M PHY (1.0 Mbps)                                                                           |
|     1 Data Bit ───► [ 1 Radio Symbol ] ───► Standard Range (~10-30m) (Stereo Audio Default)       |
|                                                                                                    |
|  2. LE 2M PHY (2.0 Mbps)                                                                           |
|     1 Data Bit ───► [ 0.5 µs Symbol ]  ───► Halved Airtime, Multi-Channel Capacity (5-6 BIS)        |
|                                                                                                    |
|  3. LE Coded PHY (125 / 500 kbps)                                                                  |
|     1 Data Bit ───► [ FEC + Spreader ] ───► 2x / 8x Symbols ───► Long Range Beacons & Control     |
+----------------------------------------------------------------------------------------------------+
```

### A. LE 1M PHY (`1.0 Mbps` - Uncoded Standard)
* **Modulation**: 1.0 Mega-symbols/second (1 Msym/s) GFSK.
* **Bit-to-Symbol Mapping**: 1 data bit per radio symbol (**1:1 Uncoded**). No Forward Error Correction (FEC) overhead.
* **Sensitivity & Range**: ~ -97 dBm sensitivity on ESP32-C6 / Nordic SoCs. Typical indoor coverage of **10–30 meters** (line-of-sight ~50–100m).
* **Airtime**: Transmitting a 100-byte LC3 frame takes **~1000 µs (1.0 ms)**.
* **Role in Auracast**: Standard baseline for **1–2 channel stereo broadcasts** and legacy compatibility with entry-level BLE receivers.

### B. LE 2M PHY (`2.0 Mbps` - High-Speed Multi-Channel Engine)
* **Modulation**: 2.0 Mega-symbols/second (2 Msym/s) GFSK.
* **Halved Packet Airtime**: Transmitting a 100-byte LC3 frame takes only **~500 µs (0.5 ms)**.
* **Multi-Channel Capacity**: In an Isochronous Group (BIG) with 5–6 discrete BIS streams (e.g. 5.1 surround sound), all channels must fit within a single 10 ms audio interval. LE 2M consumes only **~2.5 ms for 5 channels**, compared to ~5.0 ms on LE 1M.
* **Dropout-Free Retransmissions (RTN)**: Because Auracast is connectionless (no ACK/NACK from sinks), the broadcaster sends redundant packet bursts (**RTN = 2 to 4**) across different frequency channels. LE 2M provides ample remaining airtime in each 10 ms window to retransmit every audio channel 2–4 times, dramatically reducing Wi-Fi interference dropouts.
* **Sensitivity & Range**: ~ -94 dBm sensitivity (~3 dB lower link budget than 1M), yielding **15–20 meters indoor coverage**, perfectly suited for room/stage listening.

### C. LE Coded PHY (`125 kbps / 500 kbps` - Long-Range Discovery & Control)
* **Modulation**: 1.0 Msym/s GFSK with **Forward Error Correction (FEC)** convolutional coding and pattern spreading:
  * **`S=2` (500 kbps)**: 1 data bit encoded into 2 symbols (2x redundancy) -> **~2x range gain**.
  * **`S=8` (125 kbps)**: 1 data bit encoded into 8 symbols (8x redundancy) -> **~4x to 8x range gain (up to 500m–1000m line-of-sight)**.
* **Hardware Viterbi Decoding**: Receiver sensitivity improves to **`-105 dBm`**, allowing the radio to reconstruct weak signals buried below the noise floor.
* **Role in High-Fidelity Audio Systems**: Due to its 125/500 kbps bandwidth ceiling, **LE Coded is not used for multi-channel audio data payloads**, but serves three vital supporting functions:
  1. **Long-Range Discovery Beacons**: Public Broadcast Announcements (PBA) and Basic Audio Announcements (BASE) broadcast on Coded PHY can be discovered by receivers hundreds of meters away before entering audio range.
  2. **Unbreakable GATT Control Plane**: Volume Control (VCS) and Scan Delegation (BASS) links running over Coded PHY maintain connectivity even if a speaker is positioned at the extreme venue perimeter.
  3. **Emergency Voice Fallback**: High-priority low-bitrate mono voice (16 kHz LC3 @ 24 kbps) can fit inside Coded PHY for emergency paging over extreme distances.

---

### Optimal PHY Allocation Matrix for High-Fidelity Multi-Channel Auracast

| Broadcast Stream / Link Component | Target PHY | Rationale & Architectural Benefit |
| :--- | :--- | :--- |
| **Broadcast Isochronous Streams (BIS 1–6)** | **`LE 2M PHY`** | High throughput, lowest latency, supports 5.1 surround sound with RTN=2-4 retransmissions. |
| **Periodic Advertising (BigInfo / BASE)** | **`LE 1M PHY`** | Universal synchronization compatibility across all Bluetooth SIG compliant receivers. |
| **Extended Advertising Discovery Beacons** | **`LE 1M` / `LE Coded`** | Dual-PHY beaconing for both standard room discovery (1M) and long-range announcement (Coded). |
| **GATT Control Plane (VCS, BASS, PACS)** | **`LE 1M` / `LE Coded`** | Ultra-reliable, low-power parameter exchange and telemetry reporting. |

---

## BLE Audio (LC3) Stereo Configurations & Bandwidth Matrix

In Bluetooth LE Audio (BAP / LC3), the **Sampling Rate**, **Frame Duration**, and **Octets per Frame (Compression Target)** are fully configurable and independent parameters defined in BAP PAC (Published Audio Capabilities) descriptors.

The table below summarizes the bandwidth, packet size, transmission cadence, and PCM buffer metrics across standard stereo configurations:

> **Note**: Metrics assume **16-bit Stereo (Left + Right)** audio channels.

| Sample Rate | Frame Duration | Packet Cadence | PCM Samples / Frame (per ch) | Raw PCM Size / Frame (Stereo) | LC3 Octets / Frame (per ch) | LC3 Compressed Packet Size (Stereo) | LC3 Stereo Bitrate (Bandwidth) | Raw PCM Bitrate (Uncompressed) | Compression Ratio | Standard BAP Quality Target |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **24.0 kHz** | **10.0 ms** | 100 fps | 240 | 960 bytes | **60 octets** | **120 bytes** | **96.0 kbps** | 768.0 kbps | 8.0 : 1 | Voice / Low Bandwidth |
| **24.0 kHz** | **10.0 ms** | 100 fps | 240 | 960 bytes | **100 octets** | **200 bytes** | **160.0 kbps** | 768.0 kbps | 4.8 : 1 | High Quality Speech |
| **24.0 kHz** | **10.0 ms** | 100 fps | 240 | 960 bytes | **120 octets** | **240 bytes** | **192.0 kbps** | 768.0 kbps | 4.0 : 1 | High Quality Speech |
| **24.0 kHz** | **7.5 ms** | 133.3 fps | 180 | 720 bytes | **60 octets** | **120 bytes** | **128.0 kbps** | 768.0 kbps | 6.0 : 1 | Low-latency Voice |
| **24.0 kHz** | **7.5 ms** | 133.3 fps | 180 | 720 bytes | **100 octets** | **200 bytes** | **213.3 kbps** | 768.0 kbps | 3.6 : 1 | Low-latency Speech |
| **24.0 kHz** | **7.5 ms** | 133.3 fps | 180 | 720 bytes | **120 octets** | **240 bytes** | **256.0 kbps** | 768.0 kbps | 3.0 : 1 | Ultra-transparent Voice |
| **32.0 kHz** | **10.0 ms** | 100 fps | 320 | 1,280 bytes | **60 octets** | **120 bytes** | **96.0 kbps** | 1,024.0 kbps | 10.7 : 1 | Standard Media (Low bit) |
| **32.0 kHz** | **10.0 ms** | 100 fps | 320 | 1,280 bytes | **100 octets** | **200 bytes** | **160.0 kbps** | 1,024.0 kbps | 6.4 : 1 | Balanced Media |
| **32.0 kHz** | **10.0 ms** | 100 fps | 320 | 1,280 bytes | **120 octets** | **240 bytes** | **192.0 kbps** | 1,024.0 kbps | 5.3 : 1 | High Quality Broadcast |
| **32.0 kHz** | **7.5 ms** | 133.3 fps | 240 | 960 bytes | **60 octets** | **120 bytes** | **128.0 kbps** | 1,024.0 kbps | 8.0 : 1 | Low-latency Media |
| **32.0 kHz** | **7.5 ms** | 133.3 fps | 240 | 960 bytes | **100 octets** | **200 bytes** | **213.3 kbps** | 1,024.0 kbps | 4.8 : 1 | Gaming / Interactive |
| **32.0 kHz** | **7.5 ms** | 133.3 fps | 240 | 960 bytes | **120 octets** | **240 bytes** | **256.0 kbps** | 1,024.0 kbps | 4.0 : 1 | High-Q Low-latency |
| **44.1 kHz** | **10.0 ms** | 100 fps | 441 | 1,764 bytes | **60 octets** | **120 bytes** | **96.0 kbps** | 1,411.2 kbps | 14.7 : 1 | CD Audio (Compressed) |
| **44.1 kHz** | **10.0 ms** | 100 fps | 441 | 1,764 bytes | **100 octets** | **200 bytes** | **160.0 kbps** | 1,411.2 kbps | 8.8 : 1 | CD Audio (Standard BAP) |
| **44.1 kHz** | **10.0 ms** | 100 fps | 441 | 1,764 bytes | **120 octets** | **240 bytes** | **192.0 kbps** | 1,411.2 kbps | 7.4 : 1 | CD Audio (High-Fidelity) |
| **44.1 kHz** | **7.5 ms** | 133.3 fps | 331 | 1,324 bytes | **60 octets** | **120 bytes** | **128.0 kbps** | 1,411.2 kbps | 11.0 : 1 | Low-latency CD Media |
| **44.1 kHz** | **7.5 ms** | 133.3 fps | 331 | 1,324 bytes | **100 octets** | **200 bytes** | **213.3 kbps** | 1,411.2 kbps | 6.6 : 1 | Low-latency CD Media |
| **44.1 kHz** | **7.5 ms** | 133.3 fps | 331 | 1,324 bytes | **120 octets** | **240 bytes** | **256.0 kbps** | 1,411.2 kbps | 5.5 : 1 | Studio Low-latency |
| **48.0 kHz** | **10.0 ms** | 100 fps | 480 | 1,920 bytes | **60 octets** | **120 bytes** | **96.0 kbps** | 1,536.0 kbps | 16.0 : 1 | Auracast Public Audio |
| **48.0 kHz** | **10.0 ms** | 100 fps | 480 | 1,920 bytes | **100 octets** | **200 bytes** | **160.0 kbps** | 1,536.0 kbps | 9.6 : 1 | Auracast High-Quality |
| **48.0 kHz** | **10.0 ms** | 100 fps | 480 | 1,920 bytes | **120 octets** | **240 bytes** | **192.0 kbps** | 1,536.0 kbps | 8.0 : 1 | Auracast Studio Master |
| **48.0 kHz** | **7.5 ms** | 133.3 fps | 360 | 1,440 bytes | **60 octets** | **120 bytes** | **128.0 kbps** | 1,536.0 kbps | 12.0 : 1 | Pro Gaming / Live Ear |
| **48.0 kHz** | **7.5 ms** | 133.3 fps | 360 | 1,440 bytes | **100 octets** | **200 bytes** | **213.3 kbps** | 1,536.0 kbps | 7.2 : 1 | Live Stage Monitoring |
| **48.0 kHz** | **7.5 ms** | 133.3 fps | 360 | 1,440 bytes | **120 octets** | **240 bytes** | **256.0 kbps** | 1,536.0 kbps | 6.0 : 1 | Ultra-Low Latency Studio |

### Key System Design Principles

1. **Bitrate Calculation**:
   - Stereo Bitrate (bps) = `(Octets_per_channel * 8 * 2) / Frame_Duration_Seconds`.
   - For **10.0 ms** frames: 60/100/120 octets per channel yield **96 kbps**, **160 kbps**, and **192 kbps** stereo streams.
   - For **7.5 ms** frames (transmitting at 133.3 packets/s): 60/100/120 octets per channel yield **128 kbps**, **213.3 kbps**, and **256 kbps** stereo streams.

2. **Latency vs. Overhead Tradeoff**:
   - **7.5 ms Frame Duration**: Delivers minimum algorithmic buffering delay (~22 ms physical pipeline), but increases packet transmission frequency to **133.3 packets/s**, consuming more radio airtime.
   - **10.0 ms Frame Duration**: The Auracast broadcast standard. Provides optimal RF efficiency at **100 packets/s** and aligns cleanly with 10 ms FreeRTOS ticks and I2S DMA double-buffering.

3. **Single-PDU DLE Compatibility**:
   - In Auracast Periodic Advertising and ISO BIS, all frames must fit inside the 251-byte Bluetooth 5.0 LE Data Length Extension (DLE) maximum PDU.
   - Even at the highest quality (120 octets/ch Stereo = 240 bytes), the packet fits within a single non-fragmented 251-byte PDU.


## Defining three reference encoding levels

| Level | Sample Rate | Frame Duration | Bytes per Packet | Bitrate | Detail |
|---|---|---|---|---|---|
| **High** | **48 kHz** | **7.5 ms** | **120 bytes** | **128 kbps** | High-quality, transparent audio with a low algorithmic delay of 11.5 ms. Fully transparent for the vast majority of music. |
| **Medium** | **32 kHz** | **10 ms** | **80 bytes** | **64 kbps** | Standard target for excellent fidelity in constrained bandwidth scenarios. Sounds comparable to or better than a 160–192 kbps MP3. |
| **Low** | **16 kHz** | **10 ms** | **40 bytes** | **32 kbps** | Highly efficient and typically reserved for voice applications or low-bitrate environments. |

### Measured Hardware Performance: ESP32-C6 (160 MHz Single-Core RISC-V)

| Level | Codec Engine | Frame Time (Avg) | P95 Peak | Single-Core CPU Load (%) | Real-time Headroom | Single-Core Streaming Viability |
|---|---|---|---|---|---|---|
| **High** (48k / 7.5ms / 120B) | Fixed-Point | **6.43 ms** | 6.98 ms | **85.8%** | +1.07 ms (14.2%) | **High Risk** (Packet drops during RF transmit) |
| **Medium** (32k / 10ms / 80B) | Fixed-Point | **5.55 ms** | 6.31 ms | **55.5%** | +4.45 ms (44.5%) | **Optimal & Stable (Recommended Standard)** |
| **Low** (16k / 10ms / 40B) | Fixed-Point | **3.89 ms** | 4.53 ms | **38.9%** | +6.11 ms (61.1%) | **Ultra-Low Overhead** |

### Measured Hardware Performance: ESP32-WROOM-32 (240 MHz Dual-Core Xtensa LX6)

| Level | Codec Engine | Frame Time (Avg) | P95 Peak | Single-Core CPU Load (%) | Real-time Headroom | Viability & Core Allocation |
|---|---|---|---|---|---|---|
| **High** (48k / 7.5ms / 120B) | Fixed-Point | **7.36 ms** | 7.76 ms | **98.2%** | +0.14 ms (1.8%) | Starvation risk on single core |
| **High** (48k / 7.5ms / 120B) | **Float (Hardware FPU)** | **6.00 ms** | 6.45 ms | **80.0%** | +1.50 ms (20.0%) | **Viable** (Dedicated Core 1 recommended) |
| **Medium** (32k / 10ms / 80B) | Fixed-Point | **6.34 ms** | 6.78 ms | **63.4%** | +3.66 ms (36.6%) | Viable |
| **Medium** (32k / 10ms / 80B) | **Float (Hardware FPU)** | **5.46 ms** | 5.90 ms | **54.6%** | +4.54 ms (45.4%) | **Optimal (Comfortable Headroom)** |
| **Low** (16k / 10ms / 40B) | Fixed-Point | **4.47 ms** | 4.83 ms | **44.7%** | +5.53 ms (55.3%) | Optimal |
| **Low** (16k / 10ms / 40B) | **Float (Hardware FPU)** | **4.11 ms** | 4.47 ms | **41.1%** | +5.89 ms (58.9%) | **Ultra-Low Overhead** |
