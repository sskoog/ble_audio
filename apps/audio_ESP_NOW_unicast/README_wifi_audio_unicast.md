# Multi-Unicast ESP-NOW Audio Streaming Architecture & Protocol Specification

## 1. Executive Summary & Hardware Assumptions

The **`audio_ESP_NOW_unicast`** application implements a low-latency, multi-channel, multi-speaker wireless audio distribution system over 802.11 ESP-NOW unicast semantics. The network topology comprises one central **SOURCE** transmitter and **2 to 6 SINK** speaker nodes operating synchronously with microsecond-level presentation timeline alignment.

```
                  +--------------------------------------+
                  |        ESP32-S3 SOURCE (Dongle)      |
                  |  - Xtensa Dual-Core @ 240 MHz        |
                  |  - Hardware FPU liblc3 Multi-Encoder |
                  |  - Master PTS Generation             |
                  |  - Core 1 Dedicated 100 Hz Pacer     |
                  +-------------------+------------------+
                                      |
       +------------------------------+------------------------------+
       | (Unicast HT20 MCS1)          | (Unicast HT20 MCS1)          | (Unicast HT20 MCS1)
       v                              v                              v
+---------------+              +---------------+              +---------------+
| SINK 0 (Left) |              | SINK 1 (Right)|              | SINK 2..5     |
| ESP32-C6 / S3 |              | ESP32-C6 / S3 |              | ESP32-C6 / S3 |
| MAX98357A DAC |              | MAX98357A DAC |              | MAX98357A DAC |
+---------------+              +---------------+              +---------------+
```

### Hardware Constraints & Assumptions

1. **SOURCE Node**:
   - **SoC**: ESP32-S3 (Xtensa Dual-Core @ 240 MHz).
   - **Compute Capability**: Hardware Single-Precision FPU with SIMD vector extensions.
   - **Role**: Encodes 2 to 6 independent audio channels using Google `liblc3` (~3.4 ms per stereo pair at 48 kHz / 10 ms / 96 kbps) on Core 1, attaches microsecond Presentation Time Stamps (PTS), and dispatches discrete 802.11 unicast frames to each registered SINK.
2. **SINK Nodes**:
   - **SoC**: ESP32-C6 (160 MHz 32-bit RISC-V) or ESP32-S3.
   - **Role**: Receives unicast packets targeted to its configured Channel ID (`ch_id`), filters out unrelated traffic, computes local phase-locked clock offsets, decodes LC3 frames, and outputs stereo I2S PCM to a MAX98357A Class-D amplifier.

---

## 2. 802.11 Physical Layer (PHY) & Airtime Analysis

### 2.1 PHY Configuration Rationale

Standard ESP-NOW broadcast transmissions (`FF:FF:FF:FF:FF:FF`) cannot leverage 802.11n High-Throughput (HT) rates because 802.11 MAC semantics lack an acknowledgement mechanism for broadcast destinations; the Wi-Fi baseband automatically falls back to basic OFDM (6.0 Mbps).

By operating in **Multi-Unicast mode**, each packet is addressed directly to the SINK node's individual STA MAC address (`B0:A6:04:xx:xx:xx`). This enables:
- Direct negotiation of **802.11n HT20 MCS rates** (up to 72.2 Mbps).
- Mandatory hardware-level **802.11 MAC Acknowledgements (ACKs)** sent by the receiver within 16 us (SIFS).
- Automatic hardware-level MAC retransmissions (default up to 7 retries) on frame corruption or collision.

| PHY Rate Code | Modulation & Coding | Raw Bitrate | Airtime / Packet (128B) | Airtime (6 Nodes) | Channel Clearance (10ms frame) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **HT20 MCS1 (Default)** | **QPSK 1/2** | **13.0 Mbps** | **~177 us** | **~1.06 ms** | **89.4% Free** |
| HT20 MCS2 | 16-QAM 1/2 | 19.5 Mbps | ~132 us | ~0.79 ms | 92.1% Free |
| HT20 MCS3 | 16-QAM 3/4 | 26.0 Mbps | ~110 us | ~0.66 ms | 93.4% Free |
| OFDM 12M | QPSK 1/2 | 12.0 Mbps | ~188 us | ~1.13 ms | 88.7% Free |

**Configured Default**:
- **PHY Mode**: `WIFI_PHY_MODE_HT20`
- **Data Rate**: `WIFI_PHY_RATE_MCS1_LGI` (13.0 Mbps)
- **Wi-Fi Channel**: Channel 1 (2412 MHz fixed)
- **TX Power**: `+9.0 dBm` (`36` * 0.25 dBm)

### 2.2 Airtime & Retry Headroom Breakdown

For an audio stream of 48 kHz / 10 ms / 120 octets (96 kbps):
- **Payload**: 13 bytes VSAF Header + 120 bytes LC3 Payload = **128 bytes**.
- **802.11 Overhead**: 24 bytes MAC header + 4 bytes FCS = **161 bytes over-the-air**.

```
+------------------+-----------------------+----------+---------------+
| Preamble & Header| 161-Byte MAC Frame    | SIFS     | 14-Byte ACK   |
| (HT20 Preamble)  | (13.0 Mbps QPSK 1/2)  | (16 us)  | (24.0 Mbps)   |
| 36 us            | 99 us                 | 16 us    | 26 us         |
+------------------+-----------------------+----------+---------------+
|<----------------------- Total: ~177 us ---------------------------->|
```

- **2 SINK Nodes (Stereo Pair)**: 2 * 177 us = **0.35 ms** total transmission time per 10 ms frame period (**96.5% channel clearance**).
- **6 SINK Nodes (5.1 Surround)**: 6 * 177 us = **1.06 ms** total transmission time per 10 ms frame period (**89.4% channel clearance**).

**Retry Headroom**:
Even in a congested 2.4 GHz RF environment where co-located Bluetooth or Wi-Fi causes packet collisions, a failed frame will trigger an instant hardware retransmission (~177 us). With 89.4% idle channel time (8.94 ms free per 10 ms block), the system has headroom for up to **50 total hardware retransmissions per audio cycle** without delaying audio delivery or starving the network.

---

## 3. Core Software Architecture

The software architecture is built on **ESP-IDF v6.0.2** and **FreeRTOS SMP**:

```
+---------------------------------------------------------------------------------+
|                               ESP-IDF v6.0.2 STACK                              |
+----------------------------------------+----------------------------------------+
|              CORE 0                    |                 CORE 1                 |
+----------------------------------------+----------------------------------------+
| - Wi-Fi Driver & ESP-NOW Interrupts    | - Dedicated Unicast TX Pacer (100 Hz)  |
| - Diagnostics & Telemetry (10 Hz)      | - liblc3 Multi-Channel Encoder (S3)    |
| - USB Serial Interactive CLI Task      | - Lock-Free Master Presentation Clock  |
| - SINK I2S DMA Driver & State Machine  |                                        |
+----------------------------------------+----------------------------------------+
| SHARED DATA LAYER (Thread-Safe Atomic Queues, Critical Sections, SPSC Buffers)  |
+---------------------------------------------------------------------------------+
```

### 3.1 Thread-Safety & Synchronization Rules

1. **Shared Peer Table**:
   - Access to the 6-peer registry (`m_peers[]`) is guarded by a dedicated FreeRTOS critical section (`taskENTER_CRITICAL(&m_peer_mux)`).
   - Dynamic peer addition, deletion, enable/disable, and channel remapping can execute on Core 0 CLI while Core 1 unicast engine is actively streaming.
2. **Telemetry & Error Counters**:
   - All high-frequency counters (`m_tx_packets_total`, `m_tx_acks_total`, `m_rx_packets_total`, `m_fifo_underrun`, `m_plc_count`, `m_underrun_count`) use `std::atomic<uint32_t>` with `std::memory_order_relaxed` to ensure zero cache contention and zero mutex stalls on the real-time audio path.
3. **RX Audio FIFO**:
   - Implemented as a Single-Producer Single-Consumer (SPSC) ring buffer with atomic head/tail indices.
   - Pushing occurs inside the ESP-NOW Wi-Fi reception callback (high priority ISR / Wi-Fi task).
   - Popping occurs inside the FreeRTOS SINK audio task pinned to I2S DMA interrupts.

---

## 4. Centralized Streaming State Machine

State transitions are strictly managed. State modifications **can ONLY happen through `EspNowUnicastEngine::transitionTo()`**, which verifies valid transition guards and resets appropriate internal timing benchmarks.

```
                           +-------------------+
                           |        OFF        |
                           +---------+---------+
                                     |
                                     v
                           +-------------------+
             +------------>|       IDLE        |<------------+
             |             +---------+---------+             |
             |                       |                       |
      (stop / timeout)               | (start / resume)      | (stop / pause)
             |                       |                       |
             |         +-------------+-------------+         |
             |         | (Role: SOURCE)            | (Role: SINK)
             |         v                           v         |
     +-------+---------------+             +-------+---------------+
     |         CAST          |             |       SCANNING        |
     +-----------------------+             +-------+---------------+
                                                   |
                                                   | (FIFO >= Prefill Threshold)
                                                   v
                                           +-------+---------------+
                                           |        PREFILL        |
                                           +-------+---------------+
                                                   |
                                                   | (Preload 2 DMA & Phase Lock)
                                                   v
                                           +-------+---------------+
                                           |        STREAM         |
                                           +-------+---------------+
                                                   |
                                                   | (Watchdog: 20 Missing Frames)
                                                   +-------------------------+
```

### State Definitions

| State | Role | Description & Operational Behavior | Allowed Next States |
| :--- | :--- | :--- | :--- |
| **`OFF`** | Both | Hardware drivers powered down or uninitialized. | `IDLE` |
| **`IDLE`** | Both | Network engine initialized, Wi-Fi radio active on channel, audio pipeline gated. Zero packet transmission. | `CAST`, `SCANNING`, `OFF` |
| **`CAST`**| SOURCE | Actively encoding stereo/multi-channel LC3 audio at 100 Hz and dispatching unicast frames to all enabled peers. | `IDLE`, `OFF` |
| **`SCANNING`** | SINK | Radio listening on Wi-Fi channel. Incoming frames matching target `ch_id` are placed into FIFO. Audio output muted. | `PREFILL`, `IDLE`, `OFF` |
| **`PREFILL`** | SINK | Prefill threshold reached (typically 5 frames). SINK decodes first 2 frames, preloads both I2S DMA descriptors while hardware clocks are stopped, calculates presentation release time, and enables I2S hardware. | `STREAM`, `SCANNING`, `IDLE` |
| **`STREAM`** | SINK | Active playback. Synchronized to I2S DMA interrupts. Drains FIFO at 100 Hz. Executes JIT wait and PLC if packet is missed. | `SCANNING`, `IDLE`, `OFF` |

---

## 5. Unified VSAF Packet Structure & Word Alignment

To keep the protocol minimal and avoid introducing additional discriminator bytes, **both audio streaming packets and control packets share the exact same 8-byte header layout**. 

The field **`octets` (Byte 1)** acts as the unambiguous packet type discriminator:
- **`octets == 0`**: **Control Packet** (Total packet size = **8 bytes**, no payload).
- **`octets == 40..240`**: **Audio Streaming Packet** (8-byte header + `octets` bytes of LC3 audio payload). Any octet count below 40 is invalid for an LC3 audio frame.

### 5.1 Real-Time Audio Streaming Packet (`octets >= 40`)

Transmitted periodically every 10.0 ms individually to each registered SINK. In unicast mode, `ch_id` is omitted because the destination MAC address uniquely identifies the recipient, and the SOURCE manages the channel-to-node routing table.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      seq      |octets (40..240)|             flags            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            pts_us                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   LC3 Encoded Audio Payload...                |
|               (octets bytes, default 120B / 96 kbps)          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

#### Audio Header Field Specifications:
1. **`seq` (uint8_t, Byte 0)**: Rolling sequence number (0..255) for packet loss detection and PLC.
2. **`octets` (uint8_t, Byte 1)**: LC3 frame size in bytes (40 to 240 bytes; default: **120 bytes** = 96 kbps @ 48 kHz / 10 ms).
3. **`flags` (uint16_t, Bytes 2-3 - Naturally 16-bit Aligned)**:
   - **Bits 0..2 (3 bits)**: Sample Rate Code (`0`: 8k, `1`: 16k, `2`: 24k, `3`: 32k, `4`: 48k [Default], `5`: 96k). *44.1 kHz is excluded as standard LC3 10ms frame tables require integer sample multiples: 80, 160, 240, 320, 480, 960 samples*.
   - **Bits 3..4 (2 bits)**: Frame Duration Code (`0`: 10.0 ms, `1`: 7.5 ms, `2`: 5.0 ms, `3`: 2.5 ms).
   - **Bits 5..6 (2 bits)**: Codec Profile (`0`: LC3 Standard, `1`: LC3plus High-Resolution).
   - **Bits 7..15 (9 bits)**: Reserved.
4. **`pts_us` (uint32_t, Bytes 4-7 - Naturally 32-bit Aligned)**: Microsecond Presentation Time Stamp modulo $2^{32}$ (~71.58 minute rollover with seamless two's-complement modular difference).
5. **LC3 Payload (Starts at Byte 8)**: Begins on a clean **32-bit and 64-bit word boundary**. Total packet size with 120B LC3 payload = **128 bytes**.

---

### 5.2 Out-of-Band Control Packet (`octets == 0`, Draft Architecture)

Sent **on-demand only** as unicast frames with mandatory 802.11 MAC ACK. Reuses Bytes 2-7 for command parameters without adding any payload bytes:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      seq      |  octets (0x00)|    opcode     |     param     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            value                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

#### Control Field Specifications (Total Packet: Exactly 8 Bytes):
1. **`seq` (uint8_t, Byte 0)**: Rolling transaction counter (for deduplication on 802.11 retries).
2. **`octets` (uint8_t, Byte 1 = `0x00`)**: Identifies this packet as a Control frame.
3. **`opcode` (uint8_t, Byte 2)**: Command identifier:
   - `0x01` = Set Volume
   - `0x02` = Mute / Unmute
   - `0x03` = Hardware Gain (MAX98357A 3, 6, 9, 12 dB)
   - `0x04` = Set Channel Target
   - `0x05` = Ping / Latency Echo
   - `0x06` = Reboot
4. **`param` (uint8_t, Byte 3)**: Primary argument (e.g., volume level `0..100`, mute `0/1`, gain `3..12`).
5. **`value` (uint32_t, Bytes 4-7 - Naturally 32-bit Aligned)**: Secondary argument / Microsecond timestamp.

```cpp
#pragma pack(push, 1)
typedef struct {
    uint8_t  seq;        // Byte 0: Rolling sequence number / transaction counter
    uint8_t  octets;     // Byte 1: 0 = Control Packet, 40..240 = LC3 frame size
    union {
        struct {
            uint16_t flags;  // Bytes 2..3: Sample rate, duration, profile
            uint32_t pts_us; // Bytes 4..7: Presentation Time Stamp (us)
        } audio;
        struct {
            uint8_t  opcode; // Byte 2: Control Opcode
            uint8_t  param;  // Byte 3: Primary argument (Volume 0..100, Mute 0/1)
            uint32_t value;  // Bytes 4..7: Secondary argument / Timestamp
        } ctrl;
    };
    // If octets >= 40, LC3 audio payload begins at Byte 8 (32/64-bit word boundary)
} vsaf_packet_t;
#pragma pack(pop)
```

---

## 6. LC3 FIFO Queue & Pipeline Reconfiguration

### 6.1 Ingress Filtering & Verification

When an 802.11 frame arrives in the Wi-Fi callback:
1. Header length is verified (`data_len >= sizeof(vsaf_unicast_header_t)`).
2. `hdr->ch_id` is matched against the SINK's configured target channel (`m_target_channel`). Non-matching frames are discarded immediately.
3. Reception local timestamp (`rx_local_time_us = esp_timer_get_time()`) is stamped.
4. Frame is pushed into `s_rx_fifo[]`. If full, `m_fifo_overflow` increments.
5. Audio task is notified via `xTaskNotifyGive(s_audio_task_handle)`.

```
[Incoming 802.11 Frame]
         |
         v
+-----------------------+     No
| hdr->ch_id == My_CH?  | ----------> [Drop Packet]
+-----------+-----------+
         | Yes
         v
+-----------------------------------+
| Capture rx_local_time_us          |
| Push LC3 Payload + Metadata to RB |
+-----------------------------------+
         |
         v
+-----------------------------------+
| xTaskNotifyGive(audio_task)       |
+-----------------------------------+
```

### 6.2 Pipeline Reconfiguration Triggers

1. **Sample Rate / Duration Change** (`flags` mismatch):
   - Triggers full reconfiguration:
     1. Closes current LC3 decoder instance.
     2. Calls `initDecoder(new_rate, 1, new_duration, new_octets)`.
     3. Calls `m_i2s_dac->reconfigureSampleRate(new_rate, new_duration)`.
     4. Transitions state machine back to `PREFILL` to pre-charge DMA descriptors cleanly.
2. **Bitrate / Octets Change** (`octets` mismatch):
   - If `octets` changes (e.g., from 120 to 100 bytes) while sample rate remains unchanged:
   - The Espressif LC3 decoder dynamically adapts to the new `in_bytes` length without needing to restart I2S hardware clocks or interrupt audio playback.

---

## 7. I2S DMA Dual-Descriptor & Prefill Architecture

### 7.1 Ping-Pong DMA Setup

The I2S driver (`Hardware::I2sAudioDriver`) configures the ESP32 hardware DMA with **2 descriptors** (ping-pong arrangement).
- **Descriptor Length**: Exactly one 10 ms stereo audio frame (480 samples * 2 channels * 2 bytes = **1920 bytes** at 16-bit 48 kHz).
- **Interrupt Mode**: `on_sent` callback fires whenever a descriptor finishes transmission, signaling that a slot is free.

```
 DMA Ring:
 +---------------------------+---------------------------+
 |    Descriptor 0 (10ms)    |    Descriptor 1 (10ms)    |
 |  [ Currently Playing ]   |  [ Next Frame Preloaded ] |
 +---------------------------+---------------------------+
               |
               v (DMA Done Interrupt)
      "DMA Slot Free" Event
```

### 7.2 DMA Prefill Strategy on Stream Startup

To eliminate startup pops and guarantee zero DMA underruns:
1. When entering `PREFILL`, I2S hardware clocks (`BCLK` and `WS`) remain **stopped**.
2. SINK pops Frame 0, decodes LC3, and preloads Descriptor 0 using `i2s_channel_preload_data()`.
3. SINK pops Frame 1, decodes LC3, and preloads Descriptor 1 using `i2s_channel_preload_data()`.
4. SINK calls `i2s_channel_enable()` to start clocks.
5. The hardware starts playing Descriptor 0 while Descriptor 1 provides a 10 ms safety buffer.

### 7.3 Pacing & 5 ms JIT Absorption

In `STREAM` state, decoding is driven strictly by DMA empty events with temporal buffer protection:

```
[DMA Slot Free ISR]
         |
         v
[Wait for DMA Slot Sem]
         |
         v
+-------------------------------+
| Pop Frame from RX FIFO        |
+---------------+---------------+
                |
        Has Frame?
       /              Yes          No
     /                 v               v
[Decode LC3]    [Wait up to 5 ms JIT Notification]
    |                   |
    |           Received Frame?
    |              /             |            Yes          No
    |            /                 |           v               v
    |     [Decode LC3]     [Trigger PLC]
    \___________ ______________/
                v
  [Format Stereo int16 PCM]
                |
                v
  [Write 1920B to DMA Slot]
```

- **5 ms JIT Wait**: If the Wi-Fi packet has not arrived at the exact microsecond the DMA descriptor empties, the task waits up to 5 ms (`ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5))`).
- Because Descriptor 1 is currently playing for 10 ms, waiting 5 ms leaves ample margin (2.5 ms decode time) before a DMA underrun could occur.
- If no packet arrives after 5 ms, Packet Loss Concealment (PLC) synthesizes the missing frame, ensuring continuous audio without buffer underflow.

---

## 8. Multi-Bit Depth & Sample Rate Reconfiguration Matrix

| Sample Rate | Slot Mode | Bit Depth | Slot Width | Frame Samples (10ms) | DMA Descriptor Size | Bitrate (LC3 Default) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **48000 Hz** | Stereo | 16-bit | 16-bit | 480 mono / 960 stereo | **1920 bytes** | 96 kbps (120B) |
| 48000 Hz | Stereo | 24-bit | 32-bit | 480 mono / 960 stereo | **3840 bytes** | 96 kbps (120B) |
| 48000 Hz | Stereo | 32-bit | 32-bit | 480 mono / 960 stereo | **3840 bytes** | 96 kbps (120B) |
| 44100 Hz | Stereo | 16-bit | 16-bit | 441 mono / 882 stereo | **1764 bytes** | 88.2 kbps (110B) |
| 32000 Hz | Stereo | 16-bit | 16-bit | 320 mono / 640 stereo | **1280 bytes** | 64 kbps (80B) |
| 24000 Hz | Stereo | 16-bit | 16-bit | 240 mono / 480 stereo | **960 bytes** | 48 kbps (60B) |
| 16000 Hz | Stereo | 16-bit | 16-bit | 160 mono / 320 stereo | **640 bytes** | 32 kbps (40B) |

---

## 9. Interactive CLI Console Commands

The system includes a real-time ASCII command parser running on Core 0:

| Command | Description |
| :--- | :--- |
| `peer list` | Display registered SINK peers, session uptime, sent count, and ACK percentages. |
| `peer add <mac> <ch> [name]` | Register a new SINK peer (ch 0..5, e.g. `peer add B0:A6:04:99:38:44 0 Left`). |
| `peer del <mac>` | Unregister a SINK peer from the transmission list. |
| `peer enable <mac>` / `disable` | Temporarily enable or disable unicast transmissions to a specific peer. |
| `start` / `play` / `unicast` | Transition SOURCE to `CAST` / SINK to `SCANNING`. |
| `stop` / `pause` | Stop streaming and transition node to `IDLE`. |
| `phy <mc0..mc7|6m|12m|24m>` | Dynamically switch the 802.11 PHY modulation rate across all peers. |
| `octets <40..240>` | Dynamically reconfigure LC3 frame size (default: 120 bytes). |
| `sr <16k|24k|32k|44k|48k>` | Dynamically change audio sample rate across encoder and I2S DAC. |
| `gain <3|6|9|12>` | Adjust MAX98357A hardware DAC gain level in decibels. |
| `clear` / `cls` | Reset all transmission, ACK, PLC, and DMA error counters. |
| `diag` | Print full instant telemetry report. |
| `reset` / `reboot` | Reboot microcontroller. |
