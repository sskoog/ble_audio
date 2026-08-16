# Espressif ESP32 Bluetooth 5.1+ & LE Audio SoC Overview

This document provides a comprehensive technical breakdown of all **Espressif Systems SoCs** supporting **Bluetooth 5.1, 5.2, 5.3, and 5.4**, evaluating their suitability for **Bluetooth LE Audio (Auracast)**, CPU ALU capabilities, hardware FPU presence, I²S digital audio interfaces, and real-time DSP software/hardware support.

---

## 1. Master Espressif SoC Feature & Capability Matrix

| SoC Model | Bluetooth Core | LE Audio ISOC (BIS/BIG)? | CPU Architecture & Max Frequency | CPU Bit Length | Hardware FPU? | DSP Math Acceleration | Hardware I²S Ports & Data Lanes | Wi-Fi Version | Primary Auracast Role |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **ESP32-C6** | **Bluetooth 5.3 (LE)** | **YES** (ESP-IDF v5.1+) | Single 160 MHz RISC-V (RV32IMAC) + 20 MHz LP | **32-bit** | **NO** (Software `soft-fp`) | RISC-V 'M' 32-bit HW Multiplier | **1x I²S** (1 DIN / 1 DOUT lane) | **Wi-Fi 6** (802.11ax) | **Wi-Fi 6 + Auracast Receiver/Sink Node** |
| **ESP32-H2** | **Bluetooth 5.3 (LE)** | **YES** (ESP-IDF) | Single 96 MHz RISC-V (RV32IMAC) | **32-bit** | **NO** (Software `soft-fp`) | RISC-V 'M' 32-bit HW Multiplier | **1x I²S** (1 DIN / 1 DOUT lane) | None (802.15.4 Only) | **Ultra-Low Cost Auracast Receiver Node** |
| **ESP32-H4** | **Bluetooth 5.4 (LE)** | **YES** (Native PAwR) | Dual-Core 96 MHz RISC-V | **32-bit** | **NO** (Software `soft-fp`) | RISC-V 'M' 32-bit HW Multipliers | **1x I²S** (1 DIN / 1 DOUT lane) | None (802.15.4 Only) | **Next-Gen BT 5.4 Receiver/Transmitter** |
| **ESP32-S31** | **Bluetooth 5.4 (Dual Mode)** | **YES** (Native ISOC) | Dual-Core 320 MHz RISC-V | **32-bit** | **YES** (Single-Precision) | RISC-V Vector/DSP Extensions | **Multiple I²S** | **Wi-Fi 6** (802.11ax) | **High-End LE Audio & Multimedia Hub** |
| **ESP32-C3** | Bluetooth 5.0 (LE) | **NO** (Lacks HW ISOC) | Single 160 MHz RISC-V (RV32IMC) | **32-bit** | **NO** (Software `soft-fp`) | RISC-V 'M' 32-bit HW Multiplier | **1x I²S** (1 DIN / 1 DOUT lane) | Wi-Fi 4 (802.11b/g/n) | Standard IoT Node (Not for LE Audio) |
| **ESP32-S3** | Bluetooth 5.0 (LE) | **NO** (Lacks HW ISOC) | Dual-Core 240 MHz Xtensa LX7 | **32-bit** | **YES** (Single-Precision FPU)| 128-bit Xtensa Vector SIMD | **2x I²S** (Dual-lane: 2 DIN / 2 DOUT per port) | Wi-Fi 4 (802.11b/g/n) | **Master Hub DSP & Controller** (Needs Ext BT Module) |
| **ESP32-P4** | No Radio Onboard | N/A (No Radio) | Dual 400 MHz RISC-V (HP) + 40 MHz LP | **32-bit** | **YES** (Hardware FPU) | RISC-V Vector SIMD Extensions | **3x I²S / TDM** (Multi-lane: 4 DIN / 4 DOUT + TDM) | None (Requires Ext Radio) | **High-End Audio DSP Engine** |
| **nRF5340** | **Bluetooth 5.4 (LE)** | **YES** (Native) | Dual Arm Cortex-M33 (128MHz App + 64MHz Net)| **32-bit** | **YES** (FPv5-SP Single Precision)| Arm SIMD Single-Cycle MAC | **1x I²S** (1 SDIN / 1 SDOUT lane) | None | **Open-Source Zephyr LE Audio Node** |

- [Dedicated Nordic Semiconductor nRF5340 LE Audio Specification](nRF5340_ble_audio.md)


<img src="./images/ESP32_feature_overview.png" width=700>


---

## 2. Detailed Pros & Cons Breakdown by SoC Model

### 1. ESP32-C6 (Wi-Fi 6 + Bluetooth 5.3 LE Audio)

The **ESP32-C6** is Espressif's flagship single-chip solution combining **Wi-Fi 6 (802.11ax)**, **Bluetooth 5.3 LE**, and 802.15.4 (Thread/Zigbee) powered by a 160 MHz 32-bit RISC-V core.

```
+-------------------------------------------------------------------------------+
|                            ESP32-C6 System Block                              |
|                                                                               |
|  +-----------------------+     +-------------------+     +-----------------+  |
|  | 160 MHz RISC-V Core   |     | Bluetooth 5.3     |     | Wi-Fi 6         |  |
|  | 32-bit Integer ALU    |     | Hardware ISOC/BIS |     | 802.11ax        |  |
|  +-----------------------+     +-------------------+     +-----------------+  |
|                                                                               |
|  * I²S Digital Output ──> External I²S DAC (PCM5102A / MAX98357A)            |
|  * Software LC3 Decoding via `esp_lc3` Component                              |
+-------------------------------------------------------------------------------+
```

- **CPU & ALU Architecture:** 32-bit RISC-V (RV32IMAC) @ 160 MHz. Includes hardware 32-bit integer multiplication & division (`M` extension). **No Hardware FPU**.
- **HP (160 MHz) vs. LP (20 MHz) Dual-Core Subsystem Explained:**
  - **HP (High-Performance Core - 160 MHz):** The primary 32-bit RISC-V core that runs FreeRTOS, Wi-Fi 6, Bluetooth 5.3 LE Audio stack, `esp_lc3` software decoding, and I²S audio DMA transfers.
  - **LP (Low-Power Core - 20 MHz):** An independent ultra-low-power 32-bit RISC-V **Deep Sleep Coprocessor** running in the RTC power domain. While the main 160 MHz HP core and Wi-Fi/BLE radios are powered down in deep sleep (consuming microamps), the 20 MHz LP core remains awake to monitor low-power GPIO sensors, touch buttons, or rotary encoders. When triggered, the LP core wakes up the main 160 MHz HP core to resume audio playback.
  - *Note:* The 20 MHz LP core does **not** participate in LC3 decoding or I²S audio processing—all audio execution occurs on the 160 MHz HP core.
- **I²S Audio Ports:** 1x Hardware I²S interface supporting Full/Half-Duplex Master or Slave modes up to 24-bit/192kHz PCM audio.
- **BLE Audio Stack:** Full native support in **ESP-IDF v5.1+** (NimBLE and Bluedroid LE Audio stacks) for Isochronous Channels (CIS/BIS), Periodic Advertising (PA), BASS, and VCS profiles.
- **DSP Hardware/Software:** Uses Espressif's optimized `esp_lc3` fixed-point integer library for LC3 decoding. Supports `esp-dsp` fixed-point math.

#### Pros:
- **Integrated Wi-Fi 6 + Auracast:** Single-chip solution capable of receiving Wi-Fi streams (AirPlay, Spotify Connect) and broadcasting/receiving Auracast BIS streams.
- **Extremely Low Cost:** Dev boards available for **~$4.00 USD** (`ESP32-C6-DevKitC-1`).
- **Unified ESP-IDF C/C++ Ecosystem:** Re-uses standard FreeRTOS, ESP-IDF drivers, and toolchains.

#### Cons:
- **Single Core:** The 160 MHz RISC-V core must handle BLE radio interrupts, LC3 decoding, and audio buffer servicing on a single core.
- **NO Hardware FPU:** Real-time DSP filtering (e.g. Hilbert transforms, biquad EQ) **must be coded in 32-bit fixed-point integer math**. Attempting to use `float` variables introduces a severe software floating-point (`soft-fp`) performance penalty (40–60 clock cycles per operation) that causes audio stuttering.

---

### 2. ESP32-H2 (Ultra-Low Power Bluetooth 5.3 LE Audio)

The **ESP32-H2** is Espressif's dedicated low-power Bluetooth 5.3 LE + 802.15.4 SoC powered by a 96 MHz 32-bit RISC-V core.

- **CPU & ALU Architecture:** 32-bit RISC-V (RV32IMAC) @ 96 MHz. **No Hardware FPU**.
- **I²S Audio Ports:** 1x Hardware I²S interface (requires external I²S DAC).
- **BLE Audio Stack:** Native BLE 5.3 ISOC/BIS channel support in ESP-IDF.
- **DSP Hardware/Software:** Fixed-point `esp_lc3` decoder.

#### Pros:
- **Lowest Unit Cost:** SoCs cost **~$2.00 USD**, dev boards **~$3.00 USD**.
- **Ultra-Low Power Consumption:** Excellent for battery-powered Auracast receiver earbuds or portable active speaker nodes.

#### Cons:
- **Lower Clock Frequency (96 MHz):** Tight CPU margin when running LC3 decoding alongside complex audio filters.
- **Single Core & No FPU:** Requires fixed-point DSP math; vulnerable to single-core interrupt delays.
- **No Wi-Fi:** Bluetooth 5.3 and 802.15.4 Thread/Zigbee only.

---

### 3. ESP32-H4 (Next-Gen Bluetooth 5.4 Dual-Core RISC-V)

The **ESP32-H4** is Espressif's next-generation Bluetooth 5.4 LE Audio SoC announced in 2026.

- **CPU & ALU Architecture:** **Dual-Core 32-bit RISC-V @ 96 MHz**. **No Hardware FPU**.
- **I²S Audio Ports:** 1x Hardware I²S interface.
- **BLE Audio Stack:** Native Bluetooth 5.4 with Periodic Advertising with Responses (PAwR) and direction-finding (AoA/AoD).

#### Pros:
- **Dual RISC-V Cores:** Eliminates single-core contention! Core 0 handles BLE radio ISRs and LC3 decoding, while Core 1 services I²S DMA buffers and audio DSP routines.
- **Bluetooth 5.4 Native:** Adds PAwR support for two-way metadata control.

#### Cons:
- **No Hardware FPU:** Still relies on 32-bit fixed-point integer arithmetic for DSP math.
- **Early Sampling Phase:** Development kits (`ESP32-H4-DevKitC-1`) are currently in early engineering sampling.

---

### 4. ESP32-S31 (High-End Wi-Fi 6 & Bluetooth 5.4 LE Audio Hub)

The **ESP32-S31** is Espressif's high-performance SoC released in 2026, targeting advanced AIoT, multimedia, and audio streaming applications. *(Note: Sometimes mistakenly referred to as ESP32-C31).*

- **CPU & ALU Architecture:** **Dual-Core 32-bit RISC-V @ 320 MHz**. Features a **Hardware Single-Precision FPU** and RISC-V Vector/DSP instructions.
- **I²S Audio Ports:** Multiple Hardware I²S interfaces with multi-lane support.
- **BLE Audio Stack:** **Bluetooth 5.4 Dual Mode** (Classic BR/EDR + LE Audio). Fully supports hardware Isochronous Channels (ISOC/BIS/CIS).

#### Pros:
- **Massive Computing Power:** The dual 320 MHz cores and hardware FPU provide incredible headroom for complex DSP, multi-channel LC3 decoding, and AI audio processing without stuttering.
- **True Dual-Mode Bluetooth 5.4:** Can operate as a classic Bluetooth A2DP receiver AND an Auracast LE Audio broadcaster simultaneously.
- **Wi-Fi 6 Integrated:** Perfect for high-bandwidth Wi-Fi audio streaming (AirPlay, DLNA) acting as a bridge to Auracast.

#### Cons:
- **Higher Power Consumption & Cost:** Not ideal for tiny, battery-operated earbuds compared to the ultra-low-power H2 or H4 series.

---

### 5. ESP32-S3 (Mainstream Master Controller — No Native Auracast Radio)

The **ESP32-S3** is Espressif's flagship dual-core 240 MHz Xtensa LX7 SoC.

- **CPU & ALU Architecture:** **Dual-Core 32-bit Xtensa LX7 @ 240 MHz**. Features a **Hardware Single-Precision FPU** and **128-bit Vector SIMD instructions**.
- **I²S Audio Ports:** **2x Hardware I²S interfaces** (Full-Duplex Master/Slave).
- **BLE Subsystem:** **Bluetooth 5.0 LE Data ONLY (Lacks Hardware Isochronous Channels ISOC/BIS!)**

> [!IMPORTANT]
> **Can Hardware ISOC / BIS Channels Be Emulated in Software on ESP32-S3?**  
> **NO, not for standard Bluetooth SIG Auracast compatibility.**  
>   
> **Why Software Emulation Fails:**  
> 1. **Silicon Baseband Limitations:** Isochronous Channels (ISOC, BIS, CIS) are **hardware-level silicon features** embedded in the 2.4 GHz radio baseband modulator. They require microsecond-accurate hardware Link-Layer timing engines ("Anchor Points") and dedicated HCI ISO DMA queues that do not exist in the ESP32-S3's Bluetooth 5.0 radio silicon.  
> 2. **Modem Packet Rejection:** The ESP32-S3 Bluetooth modem hardware discards Bluetooth 5.2+ Isochronous PDU frames at the hardware baseband level before software ever sees them.  
> 3. **Proprietary BLE Adv Workaround vs. Auracast:** While a developer could theoretically pack custom audio data into standard Bluetooth 5.0 Advertising frames for a closed ESP32-to-ESP32 link, it will **NOT be recognized by commercial Auracast devices** (Galaxy Buds, hearing aids, Auracast receivers), lacks microsecond inter-speaker time synchronization, and suffers from severe advertising jitter.

---

### 6. FreeRTOS Task Scheduling & Core Assignment Realities

A common point of confusion when looking at the ESP32-C6 specs is whether both the **160 MHz HP core** and the **20 MHz LP core** are available to run FreeRTOS tasks simultaneously.

#### Are Both Cores Available for FreeRTOS Task Pinning (`xTaskCreatePinnedToCore`)?

**NO.** On the ESP32-C6, FreeRTOS runs **EXCLUSIVELY as a Unicore (Single-Core) OS on the 160 MHz HP core**:

1. **FreeRTOS Unicore Mode (`CONFIG_FREERTOS_UNICORE=y`):**  
   ESP-IDF configures the ESP32-C6 as a single-core target. Calling `xTaskCreate()` or `xTaskCreatePinnedToCore()` assigns all application threads, BLE radio stacks, and audio tasks to **Core 0 (the 160 MHz HP core)**.
2. **How the 20 MHz LP Core Operates:**  
   The LP core does **NOT** run the FreeRTOS scheduler, cannot access main DRAM, and cannot call standard ESP-IDF APIs (like Wi-Fi, Bluetooth, queues, or `printf`). It runs a completely separate, bare-metal C program (compiled via `ulp_lp_core` C toolchain) that executes inside the isolated RTC memory domain during deep sleep.

#### FreeRTOS Multi-Core Task Pinning Matrix Across ESP32 SoCs

| SoC Model | FreeRTOS Scheduler Type | Cores Available for FreeRTOS Tasks | Multi-Core Task Pinning (`xTaskCreatePinnedToCore`) | Audio/BLE Task Isolation Capability |
| :--- | :--- | :--- | :--- | :--- |
| **ESP32-C6** | **Unicore (Single Core)** | **Core 0 Only (160 MHz HP RISC-V)**| Single Core Only (All tasks on Core 0) | NO (BLE & Audio share 1 core) |
| **ESP32-H2** | **Unicore (Single Core)** | **Core 0 Only (96 MHz RISC-V)** | Single Core Only (All tasks on Core 0) | NO (BLE & Audio share 1 core) |
| **ESP32-H4** | **SMP (Dual Core)** | **Core 0 & Core 1 (96 MHz RISC-V)**| **YES (Core 0 & Core 1 Available!)** | **YES (Isolate BLE to Core 0, Audio to Core 1)** |
| **ESP32-S3** | **SMP (Dual Core)** | **Core 0 & Core 1 (240 MHz Xtensa)**| **YES (Core 0 & Core 1 Available!)** | **YES (Isolate Wi-Fi/BLE to Core 0, DSP to Core 1)**|

---

#### Pros:
- **Exceptional Audio DSP Performance:** Hardware FPU and 128-bit Vector SIMD calculate 32-bit floating-point Hilbert transforms, fast FFTs, and multi-band PEQs effortlessly.
- **Dual 240 MHz Cores:** Massive MIPS headroom (~480 MIPS) for multi-source audio cross-fading, Wi-Fi streaming, web server, and display UI.
- **2x I²S Ports:** Allows simultaneous connection to external digital inputs and audio DACs.

#### Cons:
- **CANNOT perform native Auracast broadcasting or receiving by itself!** Lacks hardware ISOC channels in its radio controller. Must pair with an external LE Audio module (e.g., Feasycom FSC-BT1058 or Nordic nRF5340) over UART and I²S.

---

## 3. Real-Time DSP Math Performance: Floating-Point vs. Fixed-Point

When implementing real-time audio algorithms (such as Hilbert transforms, biquad PEQ crossovers, or room correction filters) alongside LE Audio LC3 decoding, the presence of a **Hardware FPU** dictates your code architecture:

```
                                Real-Time Audio DSP Execution Model
                                
  +-----------------------------------------------------------------------------------------+
  |  ESP32-S3 (Xtensa LX7 @ 240 MHz + Hardware FPU + 128-bit SIMD)                          |
  |  - Standard 32-bit `float` math executes in 1 cycle!                                    |
  |  - Ideal for Floating-Point Hilbert Transforms & Multi-band Biquad PEQs                 |
  +-----------------------------------------------------------------------------------------+
                                              vs
  +-----------------------------------------------------------------------------------------+
  |  ESP32-C6 / ESP32-H2 / ESP32-H4 (RISC-V @ 96-160 MHz, NO Hardware FPU)                   |
  |  - Standard `float` math uses software emulation (`soft-fp`) taking 40-60 cycles/op!  |
  |  - MUST use 32-bit Fixed-Point Integer Math (Q15 / Q31 via `esp-dsp`) to prevent underruns|
  +-----------------------------------------------------------------------------------------+
```

---

## 4. Hardware I²S DAC Wiring Guide for ESP32 Auracast Receiver Nodes

Because all Espressif SoCs lack an onboard audiophile analog power DAC, an **external I²S DAC or Digital Class-D Amplifier** is required to connect to the ESP32's hardware I²S interface when acting as an Auracast receiver/sink:

```
[ ESP32-C6 / ESP32-H2 Node ]                     [ External I²S Audio DAC / Amp ]
  - GPIO19 (I2S_BCLK)  ────────────────────────>  - Bit Clock (BCLK / BCK)
  - GPIO20 (I2S_WS)    ────────────────────────>  - Word Select / Frame Clock (LRCK / WS)
  - GPIO21 (I2S_DOUT)  ────────────────────────>  - Data Input (DIN / SD)
  - GND                ────────────────────────>  - Ground (GND)
  - 3.3V / 5V Power    ────────────────────────>  - Power Supply (VCC)

Recommended DAC / Amp Chips:
  1. Texas Instruments PCM5102A (24-bit/192kHz DAC Board, ~$2 USD)
  2. Maxim MAX98357A (Mono 3.2W I²S Class-D Amp Board, ~$2 USD)
  3. Texas Instruments TAS5805M (Stereo I²S Direct Class-D Amp with onboard DSP)
  4. ESS Technology ES9023 / ES9038Q2M (Audiophile DAC Boards)
```

---

## 5. Summary Recommendation Guide

1. **For Master Hub Broadcaster (Source Switcher & System DSP):**  
   Use **ESP32-S3 + Feasycom FSC-BT1058 (QCC5181)**. The ESP32-S3's dual 240 MHz Xtensa cores with hardware FPU handle source switching, gain normalization, and floating-point room EQ, while the FSC-BT1058 handles the Auracast BIG/BIS broadcast.

2. **For Single-Chip Wi-Fi 6 + Auracast Receiver Speakers:**  
   Use **ESP32-C6 + External I²S DAC (PCM5102A)**. Write all DSP crossover/Hilbert code in **32-bit fixed-point integer math** to leverage the 160 MHz RISC-V hardware multiplier.

3. **For Low-Cost Battery Powered Receiver Speaker Nodes:**  
   Use **ESP32-H2 + External I²S DAC/Amp**. An unbeatable ~$3.00 USD total BOM cost per Auracast receiver node.

---

## 6. Sharing a Single I²S Interface Across Multiple Devices

When an SoC features only **1x Hardware I²S interface** (like the ESP32-C6, ESP32-H2, or nRF5340), sharing the I²S clock and data lines across multiple audio ICs (e.g. 2x Stereo ADCs for 4-channel analog input, or 1x ADC + 1x DAC) is a critical hardware design requirement.

### 4 Methods to Share a Single I²S Interface

#### 1. Multi-Lane I²S (Shared Clock, Separate Data Lines) — Recommended for Dual ADCs
- **How it works:**  
  Both stereo ADCs share the exact same **`BCLK`** and **`WS/LRCK`** clock lines from the MCU. ADC #1 connects its data output to **`DIN_0`**, and ADC #2 connects its data output to **`DIN_1`**.
- **Result:** Both ADCs sample simultaneously on the exact same clock edge, providing **4 discrete audio channels** into the MCU DMA controller.
- **Hardware Requirement:** The MCU I²S controller must support multi-lane DMA (e.g. ESP32-S3 supports 2 data lanes; ESP32-P4 supports 4 data lanes).

#### 2. TDM Mode (Time Division Multiplexing / DSP Mode) — Recommended for >4 Channels
- **How it works:**  
  Instead of 2-channel I²S (which alternates between Left and Right in one `LRCK` frame), **TDM mode** packages 4, 8, or 16 audio channels onto **a single shared data line (`SDOUT`/`SDIN`)**.
- **Implementation:** During each frame sync pulse (`SYNC`), the single data line streams **Channel 1, 2, 3, 4...** in sequential time slots. All TDM-compatible ADCs/DACs share `BCLK`, `SYNC`, and the single `SDATA` line.
- **Supported Chips:** FSC-BT1058 (QCC5181) supports 8-slot TDM; ESP32-P4 supports 4/8-slot TDM; Cirrus Logic CS47L63 supports TDM.

#### 3. Tri-State Shared I²S Data Line (Mono Left + Mono Right)
- **How it works:**  
  Two mono I²S devices (e.g., 2x mono MEMS microphones) share the exact same `BCLK`, `WS`, and `SDATA` line. Mic #1 drives `SDATA` when `WS` is LOW (Left channel) and goes High-Impedance (Hi-Z/Tri-state) when `WS` is HIGH. Mic #2 drives `SDATA` when `WS` is HIGH (Right channel).
- **Constraint:** Standard cheap stereo ADCs (like PCM1808) keep their `DOUT` pin continuously driven High/Low (not tri-stated), so connecting two standard stereo ADC `DOUT` pins together directly will cause a **bus contention short circuit** unless tri-state control pins are present.

#### 4. Hardware Digital I²S Multiplexer (74LVC157 IC)
- **How it works:**  
  A cheap digital multiplexer IC (like the 74LVC157 quad 2-to-1 MUX) switches `BCLK`, `WS`, and `SDATA` lines between Source A (e.g. AUX Line-In ADC) and Source B (e.g. USB Audio / BT Receiver) under MCU GPIO control.
- **Result:** Allows switching between **Source A OR Source B**, but not sampling both simultaneously.

### Summary Comparison Table for Single I²S Port Sharing

| Sharing Method | Max Channels | Shared Clock Lines | Dedicated Lines Needed | Simultaneous Sampling? | Best Suited Use Case |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Multi-Lane I²S** | 4 to 8 Channels | `BCLK`, `WS/LRCK` shared | 1 extra `DIN` pin per stereo ADC | **YES** | 2x Stereo ADCs into ESP32/nRF5340 |
| **TDM Mode** | **4, 8, 16 Channels**| `BCLK`, `SYNC`, **1 Single Data Line!** | None (Single Data Line) | **YES** | Multi-channel active speaker arrays / 4.2 setups |
| **Tri-State Shared** | 2 Mono Channels | `BCLK`, `WS`, `SDATA` shared | None | **YES** | 2x Mono MEMS Mics sharing 1 line |
| **74LVC157 Digital MUX**| 2 Stereo Channels | None (Switched via MUX) | 1 MCU Select GPIO Pin | **NO** (Switches A or B) | Input Source Switching (AUX vs BT) |

