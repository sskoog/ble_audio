# Wireless DIY Multi-Speaker Design: Qualcomm Modules, Audio Codecs & Auracast Overview

This document provides a comprehensive technical guide to **Bluetooth Auracast** (LE Audio Broadcast), Bluetooth audio codec architectures, Qualcomm System-on-Chips (SoCs) supporting LE Audio, and sourcing DIY breakout modules with digital **I²S** audio output for custom wireless speaker and multi-room audio networks.

---

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

## 1.2: Link-Layer Transport: BIG and BIS
- **Broadcast Isochronous Stream (BIS):** Unidirectional logical transport stream carrying LC3 compressed audio payload frames.
  - *Channel Content & Payload:* A single BIS payload **can** technically carry a single mono channel or an interleaved stereo pair (L+R). However, under standard LE Audio / Auracast profiles (BAP / PBP), the standard configuration is **1 Mono audio channel per BIS stream** (e.g. `BIS 1` = Left, `BIS 2` = Right). This allows individual sinks (like a left earbud or a single satellite speaker node) to only decode its assigned BIS stream, reducing radio power consumption and DSP processing overhead.
- **Broadcast Isochronous Group (BIG):** A group container synchronization wrapper that groups between **1 and 31 individual BIS streams**. All BIS streams within a BIG share the exact same deterministic Link-Layer time-base, guaranteeing phase-aligned, sub-10 µs inter-channel synchronization across all 31 streams.

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


# 2: Case Studies and examples

## 2.1: BLE Audio 4.2 Wireless Multi-Speaker Setup

For a **4.2 speaker setup** (4 satellite top speakers + 2 subwoofers), two primary audio routing and control architectures exist:

### Architecture A: Stereo Broadcast + Distributed Local Node DSP (Low RF Overhead)
- **Audio Stream:** Broadcaster streams **1 BIG containing 2 BIS channels** (BIS 1 = Left, BIS 2 = Right).
- **Node Audio Routing:**
  - *Tops 1 & 2 (Left Front / Left Rear):* Lock to `BIS 1`. Local node DSP applies high-pass filter (>80 Hz), parametric EQ, and speaker alignment delay.
  - *Tops 3 & 4 (Right Front / Right Rear):* Lock to `BIS 2`. Local node DSP applies high-pass filter (>80 Hz) and parametric EQ.
  - *Subs 5 & 6 (Sub Left / Sub Right):* Lock to `BIS 1` & `BIS 2`. Local node DSP sums Left + Right to mono, applies low-pass filter (<80 Hz), sub-bass EQ, and room correction delay.
- **Continuous Control Options during Playback:**
  1. **VOCS / VCS (GATT):** Smartphone controller app connects to each node via GATT ACL link to adjust global volume (VCS) or channel gain offsets (VOCS).
  2. **PA Metadata Injection:** Broadcaster injects dynamic volume/EQ metadata into Periodic Advertising packets; sinks read PA metadata in real-time during playback to apply gain/filter coefficient updates.
  3. **Out-of-Band (OOB) Dual-Radio (ESP32-S3 ESP-NOW / Wi-Fi):** Microcontrollers run concurrent Wi-Fi / ESP-NOW mesh alongside Auracast audio to transmit ultra-low-latency (<5 ms) biquad PEQ coefficients, trim, and delay adjustments continuously.

### Architecture B: Centralized Source DSP + Multi-BIS Broadcast
- **Audio Stream:** Central broadcaster applies all 4.2 crossover filters and delay alignment at the source hub, transmitting **1 BIG containing 6 discrete BIS channels** (BIS 1..4 = Tops, BIS 5..6 = Subs).
- **Node Audio Routing:** Each sink locks strictly to its assigned BIS channel index; no local node DSP is needed.
- **Continuous Control:** Volume and DSP settings are updated continuously at the master broadcaster hub before LC3 encoding.

---

# 3. Auracast SoCs & Chipset Matrix

Qualcomm's **QCC51xx / QCC30xx** family, Nordic Semiconductor's **nRF5340**, and Espressif's latest RISC-V SoCs (**ESP32-C6, ESP32-H2, ESP32-H4**) form the primary hardware options for Bluetooth LE Audio and Auracast implementations.

### Detailed SoC Comparison Matrix

| Chip Model | Vendor / Tier | BT Version | LE Audio / Auracast | Core Architecture | Audio Codecs & Capabilities | Hardware Digital Interfaces | Target Application & Developer Model |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **QCC5181** | Qualcomm Premium | 5.4 (Dual-Mode) | **Yes** (BIS/BIG) | Dual 240 MHz Kalimba DSP | LC3, aptX Lossless (24-bit/96kHz), LDAC | I²S (Master/Slave), TDM, SPDIF, USB | High-End Speakers, Commercial Receivers (AT / ADK) |
| **QCC5171** | Qualcomm Premium | 5.3 (Dual-Mode) | **Yes** (BIS/BIG) | Dual 240 MHz Kalimba DSP | LC3, aptX Lossless, aptX HD | I²S, TDM, SPDIF, USB | Premium Headsets & Receiver Boards |
| **QCC3095** | Qualcomm Mid-Next| 5.4 | **Yes** (BIS/BIG) | Single 240 MHz Kalimba | LC3, aptX Lossless (24-bit/48kHz) | I²S, SPDIF, USB | High-Quality Receiver Modules & DACs |
| **QCC3086** | Qualcomm Broadcaster| 5.4 | **Yes** (BIS Broadcaster)| Single 240 MHz Kalimba | LC3, aptX Adaptive | I²S, USB Audio | USB-C Transmitters & Broadcast Hubs |
| **QCC3083 / 3084**| Qualcomm Mid-Tier | 5.4 | **Yes** (BIS Transceiver)| Single 240 MHz Kalimba | LC3, aptX Lossless (16-bit) | I²S, PCM, SPDIF | Mid-Tier Transceivers (FSC-BT1038A/B, BTM384) |
| **nRF5340** | Nordic Dual-Core | 5.4 / 5.3 | **Yes** (BIS/BIG Native) | Dual Arm Cortex-M33 (128MHz App + 64MHz Radio)| LC3 (Open-source Zephyr / Software Codec) | I²S (Master/Slave), PDM, USB 2.0, QSPI | **Open-Source C/C++ Dev** (Zephyr RTOS, nRF Connect SDK) |
| **ESP32-C6** | **Espressif RISC-V**| **5.3 (LE Only)** + **Wi-Fi 6** | **Yes** (BIS/BIG in ESP-IDF v5.1+)| 160 MHz RISC-V + 20 MHz LP RISC-V | LC3 (Software `esp_lc3` in ESP-IDF) | **1x Hardware I²S** (Master/Slave), SDIO, SPI | **Wi-Fi 6 + Auracast Node** (ESP-IDF C/C++, Requires Ext DAC) |
| **ESP32-H2** | **Espressif Low-Power**| **5.3 (LE Only)** + 802.15.4 | **Yes** (BIS/BIG in ESP-IDF) | 96 MHz RISC-V Single Core | LC3 (Software `esp_lc3` in ESP-IDF) | **1x Hardware I²S** (Master/Slave), USB, SPI | **Ultra-Low Cost Auracast Node** (ESP-IDF, Requires Ext DAC) |
| **ESP32-H4** | **Espressif Next-Gen**| **5.4 (LE Only)** + PAwR | **Yes** (BIS/BIG Native) | Dual RISC-V Cores | LC3 (Software `esp_lc3` in ESP-IDF) | **1x Hardware I²S** (Master/Slave), QSPI, SPI | **Next-Gen BT 5.4 LE Audio** (ESP-IDF, Requires Ext DAC) |
| **ESP32-S3** | Espressif Mainstream| 5.0 (LE Data Only) | **NO (Lacks HW ISOC/BIS)**| Dual 240 MHz Xtensa LX7 | SBC/AAC (No Native LC3/BIS) | 2x Hardware I²S, USB, SDIO | **Master Hub Controller** (Requires Ext BT Module for Auracast) |
| **QCC5125** | Qualcomm Legacy | 5.1 | **NO** (Classic Only) | Dual 120 MHz Kalimba | aptX HD, AAC, SBC (No LC3) | I²S, SPDIF | **AVOID** for Auracast (Classic Only) |
| **QCC3034 / 3031**| Qualcomm Legacy | 5.0 / 5.1 | **NO** (Proprietary) | Single 120 MHz Kalimba | Proprietary Broadcast Audio | I²S, Analog, SPDIF | **AVOID** for Auracast (Incompatible Broadcast) |
| **QCC3021** | Qualcomm Legacy | 5.0 | **NO** (Classic Only) | Single 120 MHz Kalimba | SBC, AAC | I²S, Analog | **AVOID** for Auracast (Classic Only) |

## Chip Family Breakdown

### Premium Tier (Qualcomm QCC5181 / QCC5171)
- **QCC5181:** The gold standard for DIY active speakers and broadcast transmitters. Features dual 240 MHz Kalimba DSPs, dedicated hardware cryptographic engine, native Bluetooth 5.4 LE Audio, and direct **I²S / TDM / S/PDIF** routing. Supports up to 24-bit/96kHz audio processing and Snapdragon Sound.
- **QCC5171:** Previous generation premium SoC. Supports full LE Audio/Auracast and high-resolution codecs. Commonly found on ready-made decoder breakout boards.

#### Mid-Tier Broadcasters & Transceivers (Qualcomm QCC3086 / QCC3083 / QCC3084 / QCC3095)
- **QCC3086:** Specifically configured by Qualcomm as a dedicated **Broadcast Hub Transmitter**. Powers USB-C Auracast transmitter dongles.
- **QCC3083 / QCC3084:** Mid-tier Bluetooth 5.4 SoCs supporting full LE Audio and BIS transceiving. **QCC3083** features 1-mic noise reduction (powering the Feasycom FSC-BT1038A), while **QCC3084** supports 2-mic noise cancellation / ANC (powering FSC-BT1038B and Sky Jiarun BTM384). Both expose hardware I²S and S/PDIF interfaces.
- **QCC3095:** Bridges mid-tier and premium tiers. Offers an upgraded 240 MHz Kalimba DSP with lower DAC noise floor, supporting up to 24-bit/48kHz audio.

### Nordic Semiconductor SoC Family Breakdown & Open-Source Ecosystem

Nordic Semiconductor provides the industry's premier open-source BLE Audio ecosystem via the **nRF Connect SDK (built on Zephyr RTOS)**. Unlike proprietary vendor stacks, Nordic's `nrf5340_audio` and `nrf54_audio` repositories grant 100% source-code access to LC3 encoding/decoding, BIG/BIS channel scheduling, and GATT control services (VCS, VOCS, BASS, PBP).

#### 1. High-Performance Flagship: nRF54H20
- **Multi-Core Architecture:** Powered by **Dual Arm Cortex-M33 Cores** (Application Core up to **320 MHz** with DVFS + Network Core @ **256 MHz**) coupled with multiple dedicated **128 MHz RISC-V coprocessors** for background peripheral handling and system management.
- **Memory Footprint:** **2.0 MB Non-Volatile Memory (NVM)** + **1.0 MB (1024 KB) SRAM**.
- **Radio & RF Performance:** Multiprotocol 2.4 GHz radio supporting Bluetooth 5.4 / 6.0 (Channel Sounding, LE Audio, BIS/BIG Auracast). Class-leading **+10 dBm TX power** and **-100 dBm RX sensitivity** @ 1 Mbps.
- **Peripherals & Audio Interfaces:** High-Speed USB 2.0 (480 Mbps host/device), CAN-FD, I3C, 14-bit ADC, Global RTC (System OFF mode), Hardware I²S (Master/Slave), and PDM digital mic interface.
- **Official Dev Kits & DIY Modules:** **nRF54H20-DK** / **nRF54H20-PDK**.

#### 2. Premium Ultra-Low-Power Tier: nRF54LM20A
- **Dual-Architecture Engine:** **128 MHz Arm Cortex-M33 Core** (with hardware FPU, Armv8-M DSP instructions, and Arm TrustZone) paired with a **128 MHz RISC-V coprocessor** built on a 22 nm ultra-low-leakage process node.
- **Memory Footprint:** **2.0 MB NVM** + **512 KB SRAM**.
- **Radio & RF Performance:** Bluetooth 5.4 / 6.0 Ready (Channel Sounding, LE Audio, BIS/BIG Auracast). **+8 dBm TX power** and **-104 dBm RX sensitivity**.
- **Peripherals & Audio Interfaces:** High-Speed USB 2.0 (480 Mbps), 14-bit ADC, up to 66 GPIOs, Hardware I²S, and PDM.
- **Official Dev Kits & DIY Modules:** **nRF54LM20A-DK**, Seeed Studio Xiao nRF54 (upcoming), Raytac MDBT54, Holyiot, Minewsemi.

#### 3. Scalable Ultra-Low-Power Tier: nRF54L Series (nRF54L15 / nRF54L10 / nRF54L05)
- **Shared Architecture:** Built on a 22 nm process featuring a **128 MHz Arm Cortex-M33 Core** (with hardware FPU & Arm TrustZone PSA Level 3 security) + **128 MHz RISC-V coprocessor**.
- **Scalable Memory Matrix (RRAM + RAM):**
  - **nRF54L15:** **1.5 MB RRAM** (Resistive RAM) + **256 KB SRAM** (Ideal for full LE Audio Auracast Receiver + DSP EQ + Local Storage).
  - **nRF54L10:** **1.0 MB RRAM** + **192 KB SRAM** (Mid-range cost-optimized LE Audio node).
  - **nRF54L05:** **0.5 MB RRAM** + **96 KB SRAM** (Compact LE Audio sensor/remote node).
- **Radio & Audio Interfaces:** Bluetooth 5.4 / 6.0 Ready (Channel Sounding, LE Audio, BIS/BIG Auracast), 2.4 GHz 4 Mbps proprietary mode, Hardware I²S, PDM, and high-speed SPI.
- **Official Dev Kits & 3rd-Party DIY Modules:** **nRF54L15-DK** / **nRF54L15-PDK** (which emulates the nRF54L10 and nRF54L05 variants). 3rd-party DIY castellated modules from **Raytac (MDBT54L / MDBT54L-1M)**, **Holyiot (Holyiot-24015)**, and **Minewsemi (MS88SF1)**.

#### 4. Dual-Core LE Audio Baseline: nRF5340
- **Dual-Core Architecture:** **128 MHz Arm Cortex-M33 Application Core** (1 MB Flash, 512 KB RAM, FPU/DSP) + **64 MHz Arm Cortex-M33 Network Core** (256 KB Flash, 64 KB RAM) dedicated to the 2.4 GHz radio stack.
- **Audio Interfaces:** Hardware I²S, PDM, Full-Speed USB 2.0 (12 Mbps), QSPI.
- **Official Dev Kits & 3rd-Party DIY Modules:** **nRF5340 Audio DK** (includes Cirrus Logic CS47L63 DAC/DSP onboard), **nRF5340-DK**, **Raytac MDBT53 / MDBT53V**, **Fanstel BW03**.
- [Local Nordic nRF5340 LE Audio Specification File](nRF5340_ble_audio.md)

#### 5. Legacy Baselines to Avoid for Auracast: nRF52840 & nRF52833
- **nRF52840:** 64 MHz Arm Cortex-M4F, 1.0 MB Flash, 256 KB RAM, USB 2.0 Full-Speed, Hardware I²S, PDM.
- **nRF52833:** 64 MHz Arm Cortex-M4F, 512 KB Flash, 128 KB RAM, USB 2.0 Full-Speed, Hardware I²S, PDM.
- **CRITICAL TECHNICAL LIMITATION (NO HARDWARE ISOC):** Both nRF52840 and nRF52833 **lack hardware Isochronous Baseband Channels (ISOC/BIS)** in their radio silicon modem. While they feature an I²S port and Bluetooth 5.3/5.1 data capabilities, they **cannot** run standard Bluetooth SIG Auracast / LE Audio natively. Nordic explicitly recommends migrating to nRF5340 or nRF54L/nRF54H series for LE Audio.
- **Dev Kits & DIY Breakout Modules:** **nRF52840-DK**, **nRF52833-DK**, **Seeed Studio Xiao nRF52840 / Xiao nRF52840 Sense**, **Adafruit Feather nRF52840 Express**, **Raytac MDBT50Q**, **BBC micro:bit v2** (nRF52833).

- [Local Espressif ESP32 Bluetooth 5.1+ & LE Audio Specification](espressif_esp32_ble_audio.md)

---

## SoC Architecture & CPU ALU Technical Comparison Matrix

| Architectural Metric | Qualcomm QCC5181 | Nordic nRF54H20 | Nordic nRF54LM20A | Nordic nRF54L15 / L10 / L05 | Nordic nRF5340 | Nordic nRF52840 / nRF52833 | Espressif ESP32-C6 / ESP32-H2 | Espressif ESP32-S3 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **CPU Architecture** | Dual 240 MHz Kalimba DSPs + 32/80 MHz App | Dual Arm Cortex-M33 (320MHz App + 256MHz Net) + RISC-V | 128 MHz Arm Cortex-M33 + 128 MHz RISC-V | 128 MHz Arm Cortex-M33 + 128 MHz RISC-V | Dual Arm Cortex-M33 (128MHz App + 64MHz Net) | Single 64 MHz Arm Cortex-M4F | 160 MHz / 96 MHz 32-bit RISC-V | Dual 240 MHz 32-bit Xtensa LX7 |
| **Process Node** | 22 nm | 22 nm | 22 nm | 22 nm | 40 nm | 55 nm | 40 nm | 40 nm |
| **CPU Bit Length** | **32-bit** (64-bit Acc) | **32-bit** | **32-bit** | **32-bit** | **32-bit** | **32-bit** | **32-bit** | **32-bit** |
| **Hardware FPU** | **YES** (FP & Fixed DSP SIMD) | **YES** (Dual M33 FPU + TrustZone) | **YES** (Single-Precision FPv5-SP) | **YES** (Single-Precision FPv5-SP) | **YES** (Single-Precision FPv5-SP) | **YES** (Single-Precision FPv4-SP) | **NO** (Software `soft-fp`) | **YES** (Single-Precision FPU) |
| **Onboard NVM / Flash** | External SQIF Flash | **2.0 MB NVM** | **2.0 MB NVM** | **1.5M / 1.0M / 0.5M RRAM** | 1.25 MB Flash (1M + 256K) | 1.0 MB / 512 KB Flash | **8 MB Flash (N8)** | 8 MB Flash + 8 MB PSRAM |
| **Onboard RAM** | 800 KB RAM | **1.0 MB (1024 KB)** | **512 KB SRAM** | **256K / 192K / 96K SRAM** | 576 KB RAM (512K + 64K) | 256 KB / 128 KB RAM | **512 KB / 275 KB SRAM** | 512 KB SRAM + 8 MB PSRAM |
| **Development OS** | Qualcomm ADK / AT Stack | **nRF Connect SDK (Zephyr)**| **nRF Connect SDK (Zephyr)**| **nRF Connect SDK (Zephyr)**| **nRF Connect SDK (Zephyr)**| nRF5 SDK / Zephyr RTOS | **ESP-IDF (FreeRTOS)** | **ESP-IDF (FreeRTOS)** |
| **Bluetooth Spec** | Bluetooth 5.4 Dual-Mode | Bluetooth 5.4 / 6.0 Ready | Bluetooth 5.4 / 6.0 Ready | Bluetooth 5.4 / 6.0 Ready | Bluetooth 5.4 / 5.3 LE | Bluetooth 5.3 / 5.0 | **Bluetooth 5.3 LE Only** | Bluetooth 5.0 (LE Data Only) |
| **Hardware ISOC / BIS?**| **YES (Hardware)** | **YES (Hardware)** | **YES (Hardware)** | **YES (Hardware)** | **YES (Hardware)** | **NO (Lacks HW ISOC)** | **YES (ESP-IDF v5.1+)** | **NO (Lacks HW ISOC)** |
| **LC3 Codec Engine** | Kalimba Hardware DSP | Zephyr Software LC3 | Zephyr Software LC3 | Zephyr Software LC3 | Zephyr Software LC3 | Host CPU (Proprietary) | **Software `esp_lc3` (Q31)**| Software LC3 / Host MCU |
| **Audiophile Codecs** | **aptX Lossless, LDAC** | LC3 Only | LC3 Only | LC3 Only | LC3 Only | SBC, AAC (Classic) | LC3 Only | SBC, AAC |
| **Onboard Power DAC?** | **YES (104 dB HQDAC)** | **NO** (Req Ext I²S DAC) | **NO** (Req Ext I²S DAC) | **NO** (Req Ext I²S DAC) | **NO** (Req Ext I²S DAC) | **NO** (Req Ext I²S DAC) | **NO** (Req Ext I²S DAC) | **NO** (Req Ext I²S DAC) |
| **Audio Interfaces** | 24-bit I²S (1 In/3 Out), TDM | Hardware I²S, PDM, USB HS | Hardware I²S, PDM, USB HS | Hardware I²S, PDM | Hardware I²S, PDM, USB FS | Hardware I²S (1 port), PDM | **1x Hardware I²S Interface** | 2x Hardware I²S Interfaces |
| **Official Dev Kits** | FSC-DB200 | **nRF54H20-DK** | **nRF54LM20A-DK** | **nRF54L15-DK** | **nRF5340 Audio DK** | nRF52840-DK, nRF52833-DK | **Waveshare ESP32-C6-N8** | Waveshare ESP32-S3 |
| **3rd-Party DIY Modules**| FSC-BT1058, SJR-BTM581 | nRF54H20 breakout boards | Seeed Xiao nRF54, Raytac | Raytac MDBT54L, Holyiot | Raytac MDBT53, Fanstel | Seeed Xiao nRF52840, Feather | Seeed Xiao ESP32-C6 | Seeed Xiao ESP32-S3 |
| **Approx. Chip/Module Cost**| ~13 EUR | ~$15 - $20 USD (Module) | ~$10 - $14 USD (Module) | **~$5 - $9 USD (Module)** | ~$8 - $15 USD (Module) | ~$4 - $7 USD (Module) | **~$2.00 - $4.00 USD** | ~$3.50 USD |


---

#### Legacy Chips to Avoid for Auracast (QCC3021 / QCC3031 / QCC3034 / QCC5125)
- **QCC3021 & QCC3034:** Bluetooth 5.0 / 5.1 Classic SoCs. They do not support LE Audio or Isochronous Channels (ISOC) and are restricted strictly to standard 1-to-1 Classic Bluetooth (A2DP).
- **QCC3031:** Supports Bluetooth 5.0 and an old, proprietary Qualcomm feature called *"Qualcomm Broadcast Audio"*. This is **completely incompatible** with modern, open-standard Bluetooth SIG **Auracast** (which requires Bluetooth 5.2+ LE Audio with BIS/PBP and LC3 codec). Devices built with QCC3031 cannot receive or stream Auracast broadcasts.

---

## 4. DIY Qualcomm Modules with I²S Output (AliExpress & Amazon)

For DIY hardware builders, sourcing bare-bones castellated modules or breakout boards that break out the **I²S digital audio lines (MCLK, BCLK, LRCK, DOUT)** is essential for connecting to downstream DSPs (e.g., ADAU1701, ESP32-S3) or DACs (e.g., PCM5102A, ES9023, CS4344).

```
+------------------------+                     +------------------------+
| Qualcomm QCC5181 Board |  -- I2S_BCLK ---->  |   External I2S DAC /   |
| (e.g. SJR-BTM581)      |  -- I2S_LRCK ---->  |   ESP32-S3 / DSP       |
|                        |  -- I2S_DOUT ---->  |   (PCM5102A / ADAU1701)|
|                        |  -- I2S_MCLK ---->  |                        |
+------------------------+                     +------------------------+
```

### Top DIY Qualcomm Modules with I²S


#### 1. Feasycom modules
* **FSC-BT1038A (QCC3083)**
  - [FSC-BT1038A](https://www.feasycom.com/product/fsc-bt1038a/)
  - [Alibaba FSC-BT1038A](https://www.alibaba.com/product-detail/Feasycom-FSC-BT1038A-Qualcomm-QCC3083-High_1600988743545.html)
  - [Feasystore FSC-BT1038A](https://www.feasystore.com/product/fsc-bt1038a/)
  - [Documentation](https://document.feasycom.com/docs/audio/BT1038_EN/latest/)
  - **Form Factor:** Industrial-grade castellated Bluetooth module.
  - **Digital Audio Interfaces:** I²S output, I²S input, PCM, USB.
  - **Features:** - Bluetooth 5.4 dual-mode audio receiver module
  - ~9 EUR

* **Feasycom FSC-BT1038B (Qualcomm QCC3084)** / SJR-BTM384
  - [FSC-BT1038B](https://www.feasycom.com/product/fsc-bt1038b/)
  - [Feasystore FSC-BT1038B](https://www.feasystore.com/product/fsc-bt1038b/)
  - **Form Factor:** Compact castellated surface-mount module / DIP adapter.
  - **Digital Audio Interfaces:** Hardware I²S (BCLK, LRCK, DATA, MCLK), PCM, S/PDIF.
  - **Features:** Bluetooth 5.4, LE Audio / Auracast support, low power consumption. Excellent choice   for compact multi-room receiver nodes.
  - ~10 EUR

<img src="./images/FSC_BT1038_1.webp" height="200"> <img src="./images/FSC_BT1038_3.webp" height="200"> 

* **FSC-BT1058 (QCC5181)**
  - [Feasycom FSC-BT1058](https://www.feasycom.com/product/fsc-bt1058/)
  - [Feasystore FSC-BT1058](https://www.feasystore.com/product/fsc-bt1058/)
  - [Alibaba FSC-BT1058](https://www.alibaba.com/product-detail/Feasycom-FSC-BT1058-Qualcomm-QCC5181-Bluetooth_1601272259871.html)
  - [Documentation](https://document.feasycom.com/docs/audio/BT1058_EN/latest/)
  - [User Guide](https://document.feasycom.com/docs/fsc-audio/EN/latest/bt1058/index.html)
  - [Local FSC-BT1058 Hardware & Comms Specification](fsc_bt1058.md)
  - **Form Factor:** Industrial-grade castellated Bluetooth module.
  - **Digital Audio Interfaces:** I²S output, I²S input, PCM, USB.
  - **Features:** Full Auracast broadcast & receiver stack, transparent AT command control via UART,   custom antenna options (IPEX connector or PCB antenna).
  - ~13 EUR

<img src="./images/FSC-BT1058.webp" height="200"> <img src="./images/FSC-BT1058_size.webp" height="200">

### Feasycom Technical Comparison Table: FSC-BT1058 vs. FSC-BT1038A vs. FSC-BT1038B

| Feature / Parameter | Feasycom FSC-BT1058 | Feasycom FSC-BT1038A | Feasycom FSC-BT1038B |
| :--- | :--- | :--- | :--- |
| **Base Qualcomm SoC** | **QCC5181** (Premium Tier) | **QCC3083** (Mid-Tier) | **QCC3084** (Mid-Tier) |
| **Bluetooth Core Version** | **Bluetooth 5.4 Dual-Mode** | **Bluetooth 5.4 Dual-Mode** | **Bluetooth 5.4 Dual-Mode** |
| **Auracast / LE Audio Support**| **Yes** (BIS Broadcaster & Receiver) | **Yes** (BIS Broadcaster & Receiver) | **Yes** (BIS Broadcaster & Receiver) |
| **DSP Core Architecture** | **Dual 240 MHz Kalimba DSPs** | **Single 240 MHz Kalimba DSP** | **Single 240 MHz Kalimba DSP** |
| **Developer Processor** | **32/80 MHz Core** | **32/80 MHz Core** | **32/80 MHz Core** |
| **aptX Lossless Capability** | **24-bit / 96 kHz** (Audiophile Grade) | **16-bit / 44.1 kHz** (CD Quality) | **16-bit / 44.1 kHz** (CD Quality) |
| **Supported Audio Codecs** | LC3, LC3plus, aptX, aptX HD, aptX Adaptive, aptX Lossless, LDAC, SBC, AAC | LC3, aptX, aptX HD, aptX Adaptive, aptX Lossless, SBC, AAC | LC3, aptX, aptX HD, aptX Adaptive, aptX Lossless, SBC, AAC |
| **Noise Cancellation / ANC** | 1/2-mic cVc + **Hybrid/FF/FB Active Noise Cancellation (ANC)** | 1-mic cVc Noise Reduction (No ANC) | 1/2-mic cVc + **Active Noise Cancellation (ANC)** |
| **I²S / PCM Interfaces** | **24-bit I²S (1 In, 3 Out channels up to 384 kHz)** + 8-slot TDM | 24-bit I²S (1 In, 1 Out channel) + PCM | 24-bit I²S (1 In, 1 Out channel) + PCM |
| **S/PDIF Interfaces** | **2 Instances** (Input/Output Configurable) | 1 Instance | 1 Instance |
| **Audio MCLK Output** | **Yes (Programmable MPLL)** | Yes | Yes |
| **Analog Audio Subsystem** | Stereo 24-bit HQADC, Stereo HQDAC | Stereo Line-In, Stereo Line-Out DAC | Stereo Line-In, Stereo Line-Out DAC |
| **Castellated Pad Count** | **52 Pads** (1.0mm Pitch) | 52 Pads / Compact SMT | 52 Pads / Compact SMT |
| **Module Dimensions** | **13mm x 26.9mm x 2.2mm** | 13mm x 26.9mm x 2.2mm | 13mm x 26.9mm x 2.2mm |
| **Mandatory Power Control Pin**| **`SYS_CTRL` (Pin 34 - HIGH pulse required)** | `SYS_CTRL` (Pin 34) | `SYS_CTRL` (Pin 34) |
| **Host MCU UART Control** | **Full AT Command Set (VCS, VOCS, BASS, LEAUDIO)** | **Full AT Command Set** | **Full AT Command Set** |
| **Approximate Unit Price** | **~13 EUR** | **~9 EUR** | **~10 EUR** |
| **Recommended System Role** | **Master Hub Transmitter / Audiophile Sub/Sat Receiver** | **Budget Satellite Receiver Nodes** | **Compact Receiver Nodes / ANC Headphones** |

#### 2. Nordic Semiconductor nRF5340 Hardware & Dev Modules

* **Nordic NRF5340-AUDIO-DK (Official LE Audio Development Kit)**
  - [Nordic NRF5340-AUDIO-DK Overview](https://www.nordicsemi.com/Products/Development-hardware/nRF5340-Audio-DK)
  - [nRF Connect SDK LE Audio Documentation](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/applications/nrf5340_audio/README.html)
  - **Hardware Features:** Integrated nRF5340 dual-core SoC, Cirrus Logic CS47L63 Smart Codec, USB Audio, onboard analog line in/out, digital PDM microphones, Li-Po battery charging circuit, and SEGGER J-Link onboard debugger.
  - **Software Ecosystem:** Open-source Zephyr RTOS codebase (`nrf5340_audio` application) providing complete developer control over LC3 audio frame encoding/decoding, microsecond-synced BIG/BIS broadcasting, and LE Audio GATT services (PBP, BASS, VCS, VOCS).
  - ~170 EUR

* **Low-Cost AliExpress / MinewSemi nRF5340 Dev Modules**
  - [AliExpress nRF5340 Core Module / Dev Board](https://www.aliexpress.com/item/1005006107850570.html)
  - [MinewSemi MS45SF1 nRF5340 Module](https://www.minewsemi.com/product/ms45sf1.html)
  - **Form Factor:** Compact castellated SMT breakout module / dev board.
  - **Digital Interfaces:** Hardware I²S (`SCK`, `LRCK`, `SDOUT`, `SDIN`, `MCK`), SWD Debug, USB 2.0, UART, QSPI.
  - **Features:** Ultra-low-cost entryway into open-source Zephyr LE Audio / Auracast development. Requires an external I²S DAC (e.g. PCM5102A board) for analog audio output.
  - ~8 to 15 EUR

#### 3. Espressif ESP32-C6 & ESP32-H2 Development Boards

* **ESP32-C6-DevKitC-1 (Wi-Fi 6 + Bluetooth 5.3 LE Audio)**
  - [Espressif ESP32-C6 Overview](https://www.espressif.com/en/products/socs/esp32-c6)
  - [ESP-IDF LE Audio Components](https://components.espressif.com/components/espressif/esp_lc3)
  - **Form Factor:** Standard 2.54 mm DIP breadboard development board.
  - **Digital Audio Interface:** **1x Hardware I²S Interface** (`GPIO19` BCLK, `GPIO20` WS, `GPIO21` DOUT/DIN). Must be wired to an external I²S DAC (e.g., PCM5102A board ~2 EUR) or I²S Class-D amp (MAX98357A ~2 EUR) to function as an audio slave/receiver/sink.
  - **Features:** Allows developers to re-use their existing ESP-IDF / FreeRTOS toolchains and C/C++ codebase while adding native Bluetooth 5.3 LE Audio / Auracast support.
  - ~$4.00 USD

* **ESP32-H2-DevKitM-1 (Ultra-Low Power Bluetooth 5.3 LE Audio)**
  - [Espressif ESP32-H2 Overview](https://www.espressif.com/en/products/socs/esp32-h2)
  - **Form Factor:** Compact 2.54 mm DIP development board.
  - **Digital Audio Interface:** **1x Hardware I²S Interface**. Connects to an external I²S DAC / Amplifier.
  - **Features:** Lowest cost native Auracast receiver node option.
  - ~$3.00 USD (Widely available)

* **ESP32-H4-DevKitC-1 (Next-Gen Bluetooth 5.4 LE Audio & PAwR)**
  - [Espressif ESP32-H4 Announcement](https://www.espressif.com/en/news/ESP32-H4)
  - **Hardware Features:** Dual-core 96 MHz RISC-V SoC, 320 KB SRAM, Bluetooth 5.4 LE Audio, PAwR, AoA/AoD Direction Finding, **1x Hardware I²S Interface**, USB-OTG.
  - **Availability Status (2026):** The ESP32-H4 and its official development board **`ESP32-H4-DevKitC-1`** are currently in the **engineering sampling phase** (available upon request directly via Espressif sales and select primary distributors).
  - **Prototyping Recommendation:** Because ESP-IDF uses identical LE Audio API drivers (`esp_lc3`, NimBLE/Bluedroid ISOC channels, and I²S drivers) across all RISC-V chips, code written on the widely available **`ESP32-C6-DevKitC-1`** or **`ESP32-H2-DevKitM-1`** is **100% forward-compatible** with the ESP32-H4 once general distributor stock arrives!

---

#### 4. QCC5181/QCC3095 Bluetooth Digital Interface Module
 - [QCC5181/QCC3095 Bluetooth Digital Interface Module](https://www.aliexpress.com/item/1005009728699981.html)
 - [ABKQ-QCC5181/QCC3095 Bluetooth Digital Interface](https://www.aliexpress.com/item/1005012526768486.html)
 - ~21 EUR (QCC3095)
 - ~35 EUR (QCC5181)
 - I2S output through 2.54 mm pin headers
 
 <img src="./images/QCC5181_QCC3095_module.avif" height=200>


#### 4. "Fever-grade" JCZY-QHD384 (Qualcomm QCC3084)
- [JCZY-QHD384](https://www.aliexpress.com/item/1005097903844607.html)
- [QCC3084 PCM5102 Lossless Decoder](https://www.aliexpress.com/item/1005009959523131.html)
- On-board DAC.
- Analog in, analog out. No digital output!?
- ~50 EUR

<img src="./images/JCZY-QHD384.png" height=200>

#### 5: QCC3084 + PCM5102A decode board
- AVOID the QCC5124/ QCC5125 variants (not BT5.2/Auracast)
- [QCC3084 + PCM5102A decode board](https://www.aliexpress.com/item/1005012178945789.html)
- [QCC3084 Bluetooth 5.1 +PCM5102A DAC Decode I2S USB Sound Card](https://www.aliexpress.com/item/1005008744634591.html)
- ~20 EUR

<img src="./images/QCC3084_PCM5102A_card1.avif" height=200> 
<img src="./images/QCC3084_PCM5102A_card2.avif" height=200>

#### 6. Nvarcher QCC5181 Bluetooth 5.4 LDAC Module
- [Nvarcher QCC5181 Bluetooth 5.4 LDAC Module](https://www.aliexpress.com/item/1005012764663059.html)
- ~40 EUR
- BT-antenna
- I2S output

<img src="./images/Nvarcher_QCC5181.png" height=200>

#### 7. QCC3084 Bluetooth 5.4 Receiver USB Card
- [QCC3084 Bluetooth 5.4 Receiver USB Card](https://www.aliexpress.com/item/1005008496015699.html)
- I2S output
- ~28 EUR

<img src="./images/QCC3084_receiver1.avif" height=200>  
<img src="./images/QCC3084_receiver2.avif" height=200>

#### 8. SJR-BTM581 (Qualcomm QCC5181)
- **Form Factor:** Castellated 20-pin SMT module or mounted on DIP breakout board.
- **Digital Audio Interfaces:** Exposed I²S (Master/Slave mode), S/PDIF digital output, USB Audio input.
- **Features:** Bluetooth 5.4, LE Audio / Auracast BIS receiver & transmitter, aptX Lossless, dual 240 MHz Kalimba DSP.
- **Sourcing:** Available on **AliExpress** (search: `QCC5181 I2S module` or `BTM581 Bluetooth board`).
  - ~25 EUR

<img src="./images/SJR-BTM581.avif" height="200">

#### 9: WONDOM / Sure Electronics BEB Series (QCC5171 / QCC5181 Receiver Boards)
- [WONDOM AA-AB41163](https://www.audiophonics.fr/en/bluetooth-modules-wireless-reception/wondom-aa-ab41163-beb1-p-19277.html)
- [Qcc5171 ldac lossless wireless audio receiver-beb1](https://www.aliexpress.com/item/1005012261465683.html)
- **Form Factor:** Fully assembled PCB with 3.3V LDO, USB-C power, 3.5mm AUX, and exposed pin headers for I²S/SPDIF.
- **Digital Audio Interfaces:** 5-pin I²S Header (GND, 3.3V, MCLK, BCLK, LRCK, DATA).
- **Sourcing:** Available on **Amazon**, **eBay**, and **WONDOM store** (search: `WONDOM QCC5171` or `BEB Bluetooth Audio Board`).
  - ~39 EUR

<img src="./images/wondom-aa-ab41163-beb1.jpg" height="200">

#### 10. Commercial USB-C Auracast Broadcaster Dongles (QCC3086 / QCC3084)
- **Form Factor:** Plug-and-play USB dongle with audio transmitter firmware.
- **Functionality:** Acts as the Auracast Broadcaster (hub). Plugs into PC, smartphone, or TV to stream LC3 audio over Auracast.
- **Sourcing:** Available on **Amazon** and **AliExpress** (search: `Auracast USB Transmitter` or `QCC3086 Dongle`).

<img src="./images/BT53_dongle.avif" height=200>

#### 11. Sure / WONDOM AA-AB41159 BEA1 Bluetooth 5.3 Receiver
- [AA-AB41159](https://store.sure-electronics.com/product/796)
- QCC5171
- ~30 EUR
- Receiver, Analog RCA, coaxial, optical out.

<img src="./images/AA-AB41159.jpg" height=200>

#### 12. Sure / WONDOM AA-AB41165 BDC-U Bluetooth 5.3 Receiver 
- [AA-AB41165](https://store.sure-electronics.com/product/815)
- QCC5171
- ~40 EUR
- Receiver, Analog RCA, coaxial, optical out.

<img src="./images/AA-AB41165.png" height=200>


### Top Nordic Semiconductor DIY Modules & Breakout Boards

For open-source firmware developers using **Zephyr RTOS / nRF Connect SDK**, Nordic SoCs are available in several castellated module packages and hobbyist-friendly dev breakout boards:

#### 1. Raytac MDBT54L Series (nRF54L15 / nRF54L10)
- **Base SoC:** Nordic nRF54L15 (1.5 MB RRAM + 256 KB RAM) or nRF54L10 (1.0 MB RRAM + 192 KB RAM).
- **Form Factor:** Compact castellated SMT module with PCB antenna (`MDBT54L-1M`) or u.FL/IPEX chip antenna (`MDBT54L-P1M`).
- **Interfaces:** Breaks out 128 MHz Arm Cortex-M33 GPIOs, Hardware I²S (MCLK, BCLK, LRCK, DOUT), PDM, SPI, UART, and 14-bit ADC.
- **Open-Source Support:** Supported out of the box in Zephyr RTOS target `nrf54l15dk/nrf54l15`.
- **Cost:** ~$6.00 – $9.00 USD.

#### 2. Raytac MDBT53 / MDBT53V Series (nRF5340)
- **Base SoC:** Dual-core nRF5340 (128 MHz App + 64 MHz Net Core).
- **Form Factor:** Castellated SMT module / DIP adapter breakout board.
- **Interfaces:** Hardware I²S, PDM, USB 2.0 Full-Speed, QSPI, SPI, UART.
- **Features:** Compatible with Nordic's open-source `nrf5340_audio` repository.
- **Cost:** ~$9.00 – $15.00 USD.

#### 3. Seeed Studio Xiao Series (Xiao nRF54LM20A, Xiao nRF52840 / Sense & Xiao ESP32-C6)
- **Form Factor:** Ultra-compact thumb-sized dev module (21 x 17.5 mm) with USB Type-C, LiPo battery charger, onboard reset/boot buttons, and castellated 2.54mm headers.
- **[Seeed Studio XIAO nRF54LM20A](https://www.seeedstudio.com/Seeed-Studio-XIAO-nRF54LM20A-p-6841.html):** Features Nordic **nRF54LM20A** (128 MHz Arm Cortex-M33 + 128 MHz RISC-V coprocessor, 2 MB NVM, 512 KB RAM) integrated with Nordic **nPM1300 PMIC** for advanced power management and battery charging. Supports **Bluetooth 6.0**, Channel Sounding, **LE Audio / Auracast**, Matter, Thread, and Zigbee.
- **Xiao nRF52840:** Features 64 MHz Arm Cortex-M4F, 1MB Flash, 256KB RAM, PDM mic, and 6-DOF IMU (Sense version). *Note: Lacks hardware ISOC for Auracast.*
- **Xiao ESP32-C6:** Features 160 MHz RISC-V CPU, Wi-Fi 6, and **Bluetooth 5.3 LE Audio with hardware ISOC / Auracast support**.
- **Cost:** ~$5.00 – $12.00 USD.

#### 4. Holyiot & Minewsemi Modules (Holyiot-24015 nRF54L15 & MS88SF1 nRF54L15)
- **Base SoC:** Nordic nRF54L15 (22 nm RRAM architecture).
- **Form Factor:** Low-cost castellated SMT modules for custom PCB assembly.
- **Cost:** ~$4.50 – $7.00 USD (Mouser / AliExpress).

#### 5. Raytac Official nRF54 Development Boards & AT Command Evaluation Kits

Raytac Corporation manufactures official evaluation and demo boards (`-DB-` and `-AT-UART-`) for their **nRF54L15** and **nRF54LM20A/20B** module series. These boards bridge the Nordic SoCs to standard 2.54mm pin headers and debug ports (designed to interface with an `nRF54L15-DK` PCA10156 debug probe):

##### Standard Development / Demo Boards (-DB Series)
* **AN54LV-DB-15:** Demo Board for the AN54LV-15 module (Nordic nRF54L15, integrated high-efficiency **Chip Antenna**). Breaks out 128 MHz Cortex-M33 GPIOs, hardware I²S, PDM, and SWD debug headers.
* **AN54LV-DB-K15:** Demo Board for the AN54LV-K15 module (Nordic nRF54L15, **Antenna Pin / IPEX connector** version). Optimized for testing custom external antennas and RF enclosures.
* **AN54LV-DB-U15:** Demo Board for the AN54LV-U15 module (Nordic nRF54L15, **u.FL RF connector** version). Allows direct coaxial cable connection to RF spectrum analyzers or external high-gain antennas.
* **AN54LQ-DB-15:** Demo Board for the AN54LQ-15 module (Nordic nRF54L15, **QFN castellated package**). Standard evaluation board breaking out digital audio I²S lines and high-speed SPI/UART.
* **AN54LM-DB-20A:** High-density Demo Board for the AN54LM-20A module (Nordic **nRF54LM20A**, **PCB Trace Antenna**). Breaks out up to 66 GPIOs, High-Speed USB 2.0 (480 Mbps), 14-bit ADC, and hardware I²S audio.
* **AN54LM-DB-20B:** High-density Demo Board for the AN54LM-20B module (Nordic **nRF54LM20B**, **Chip Antenna** & integrated **Axon NPU** for on-device Edge AI inferencing).

##### Pre-Flashed UART AT Command Evaluation Boards (-AT-UART Series)
* **AN54LQ-AT-UART-S:** Evaluation Board for the AN54LQ-15 module (QFN package) pre-loaded with **Peripheral / Slave UART AT Command Firmware**. Enables host microcontrollers (e.g. ESP32-S3 / STM32) to control Bluetooth LE / LE Audio functions over simple serial AT commands without embedding the full Zephyr BLE stack on the host MCU.
* **AN54LV-AT-UART-S:** Evaluation Board for the ultra-compact AN54LV-15 module (LGA package, 6.4 × 8.4 mm) pre-loaded with **Peripheral / Slave UART AT Command Firmware**. Offers ~50% lower power consumption compared to legacy nRF52 AT modules.

---




### Host MCU UART GATT Command Support Comparison

For DIY builders wanting an external host microcontroller (like an ESP32-S3 or STM32) to read and write GATT commands (such as VCS Volume, VOCS Offsets, BASS Scan commands, and device metadata), the choice of module firmware is critical:

| Module Family | Base SoC | Physical UART Pins? | MCU AT Command GATT Control? | Documented AT Spec? | Recommended for Host MCU GATT Control? |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Feasycom FSC-BT1058 / FSC-BT1057** | QCC5181 / QCC5171 | Yes (`TXD`/`RXD`) | **YES (Full AT Command Stack)** | **Yes (Public PDF Datasheet)** | **BEST CHOICE** (Native MCU UART AT Stack) |
| **Feasycom FSC-BT1038A / FSC-BT1038B** | QCC3083 / QCC3084 | Yes (`TXD`/`RXD`) | **YES (Full AT Command Stack)** | **Yes (Public PDF Datasheet)** | **EXCELLENT** (Compact Mid-Tier AT Stack) |
| **Sky Jiarun SJR-BTM581 / SJR-BTM384** | QCC5181 / QCC3084 | Yes (Castellated) | Limited (Pre-flashed standalone firmware) | Partial | Needs custom firmware or seller AT flash |
| **Generic AliExpress Decoder Boards** | QCC5181 / QCC3095 | Yes (Headers) | Standalone Only | No | Avoid for MCU GATT control (Fixed firmware) |
| **WONDOM BEB1 / BEA1 Series** | QCC5171 / QCC5181 | Yes (Header) | Standalone Only | No | Optimized for standalone DAC output |

#### Why Feasycom Modules Stand Out for Host MCU Integration:
1. **Unencrypted AT Firmware:** Feasycom writes custom module-level firmware exposing rich AT commands over UART (115200 to 921600 baud).
2. **Native GATT Commands over UART:**
   - `AT+GATT...`: Read/Write GATT characteristics on remote slaves or local GATT servers.
   - `AT+LEAUDIO=...`: Directly control LE Audio roles (Broadcaster vs. Receiver/Sink), BIG/BIS stream parameters, and BASS scan execution.
   - `AT+VOL...` / `AT+VOCS...`: Execute VCS master volume and VOCS gain offset commands over UART.
3. **No Qualcomm NDA Required:** Unlike raw Qualcomm SoCs requiring Qualcomm's ADK (Audio Development Kit) under strict commercial NDA, Feasycom modules allow complete MCU control over simple ASCII/Hex AT serial strings.

---

## 5. Hardware Integration & Microcontroller (ESP32-S3) Interfacing

Connecting a Qualcomm Auracast module to a host microcontroller (such as the **ESP32-S3**) provides full digital control, custom EQ, network management, and external DAC routing.

### Hardware Connection Topology

```
+-------------------------+             +--------------------------+
|  Qualcomm Module        |             |  ESP32-S3 Microcontroller|
|  (e.g., QCC5181 /       |             |  (Host Controller & DSP) |
|   BTM581)               |             |                          |
|                         |             |                          |
|  TXD (UART)  -----------+-----------> | RX Pin (UART Control)    |
|  RXD (UART) <-----------+-----------  | TX Pin (UART Control)    |
|                         |             |                          |
|  I2S_BCLK --------------+-----------> | GPIO (I2S Bit Clock)     |
|  I2S_LRCK --------------+-----------> | GPIO (I2S Word Select)   |
|  I2S_DOUT --------------+-----------> | GPIO (I2S Data In)       |
|  I2S_MCLK (Optional) ---+-----------> | GPIO (I2S Master Clock)  |
+-------------------------+             +--------------------------+
                                                     |
                                                     v
                                        +--------------------------+
                                        | External I2S DAC         |
                                        | (PCM5102A / ES9023)      |
                                        +--------------------------+
```

### Crucial Firmware & Hardware Integration Tips

1. **I²S Clock Configuration (Master vs. Slave Mode):**
   - By default, most pre-flashed Qualcomm modules operate as **I²S Master** (generating BCLK and LRCK).
   - Configure the receiving device (ESP32-S3 or external DAC) as **I²S Slave**.
2. **Master Clock (MCLK):**
   - Certain high-performance DACs (e.g., ESS Sabre ES9023/ES9038) require a Master Clock (MCLK), typically 12.288 MHz or 24.576 MHz. Ensure your module firmware exposes MCLK (supported on QCC5181 and QCC3095).
3. **Firmware Encryption & Vendor AT Commands:**
   - Low-cost pre-flashed modules on AliExpress carry seller-specific encrypted firmware.
   - For custom MCU control (switching Auracast channels, adjusting volume via UART), choose modules with documented AT command sets (such as **Feasycom FSC-BT1058**).

---

## 6. External Links & Authoritative References

- [Bluetooth SIG Auracast Overview & Technical Resource Center](https://www.bluetooth.com/auracast/)
- [Bluetooth SIG LE Audio Core Specification (ISOC / BIS / BIG)](https://www.bluetooth.com/specifications/le-audio/)
- [Fraunhofer IIS LC3 & LC3plus Codec Documentation](https://www.iis.fraunhofer.de/en/ff/amm/communication/lc3.html)
- [ETSI TS 103 634 LC3plus High-Resolution Specification](https://www.etsi.org/deliver/etsi_ts/103600_103699/103634/01.04.01_60/ts_103634v010401p.pdf)
- [Qualcomm Snapdragon Sound & QCC5181 Product Page](https://www.qualcomm.com/products/application/audio/qcc5100-series/qcc5181)
- [Qualcomm QCC3084 Mid-Tier Audio SoC](https://www.qualcomm.com/products/application/audio/qcc3000-series/qcc3084)
- [Feasycom Bluetooth LE Audio & Auracast Modules](https://www.feasycom.com/)
- [Search QCC5181 I2S Modules on AliExpress](https://www.aliexpress.com/w/wholesale-QCC5181-I2S-module.html)
- [Search QCC3084 I2S Modules on AliExpress](https://www.aliexpress.com/w/wholesale-QCC3084-I2S-module.html)
- [What is the QCC3091 Chip?](https://besttechradar.com/what-is-the-qcc3091-chip/)
- [QCC3072 VS QCC5171](https://www.feasycom.com/qcc3072-vs-qcc5171-bluetooth-module/)




---

## 3. Bluetooth 5.0+ Physical Layer (PHY) Modes in Auracast

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

## 4. BLE Audio (LC3) Stereo Configurations & Bandwidth Matrix

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
