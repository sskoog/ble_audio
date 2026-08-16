# Nordic Semiconductor nRF5340 Bluetooth LE Audio & Auracast Specification

This document details the hardware architecture, CPU ALU capabilities, hardware FPU presence, Bluetooth 5.4 LE Audio stack, digital audio interfaces, and development ecosystem for the **Nordic Semiconductor nRF5340** dual-core System-on-Chip (SoC).

Online Reference Documentation:
- [Nordic nRF5340 Product Specification](https://www.nordicsemi.com/Products/nRF5340)
- [Nordic nRF5340-AUDIO-DK User Guide](https://www.nordicsemi.com/Products/Development-hardware/nRF5340-Audio-DK)
- [nRF Connect SDK LE Audio Application Guide (`nrf5340_audio`)](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/applications/nrf5340_audio/README.html)

---

## 1. Module Overview & Key Specifications

The **nRF5340** is the world's first dual-core wireless SoC optimized for Bluetooth LE Audio, Auracast, and ultra-low-power IoT applications.

```
+--------------------------------------------------------------------------------+
|                        Nordic Semiconductor nRF5340                            |
|                                                                                |
|  +-------------------------------------+  +---------------------------------+  |
|  | Application Core                    |  | Network Core (Radio Engine)     |  |
|  | - 128 MHz Arm Cortex-M33              |  | - 64 MHz Arm Cortex-M33         |  |
|  | - 1 MB Flash, 512 KB RAM              |  | - 256 KB Flash, 64 KB RAM       |  |
|  | - Hardware Single-Precision FPU       |  | - Dedicated 2.4 GHz BT 5.4      |  |
|  | - Armv8-M DSP / SIMD Extensions       |  | - Hardware ISOC (BIS/BIG Engine)|  |
|  +-------------------------------------+  +---------------------------------+  |
|                                   │                         │                  |
|                                   └───────── IPC Mailbox ───┘                  |
|                                                                                |
|  * Software Stack: 100% Open-Source Zephyr RTOS & nRF Connect SDK for Audio   |
|  * Digital Audio: Hardware I²S Interface (Master/Slave), PDM Mic, USB 2.0      |
+--------------------------------------------------------------------------------+
```

### Key Technical Parameters

- **Bluetooth Version:** Bluetooth Core Specification 5.4 / 5.3 (LE Only).
- **Core Architecture:** Dual Arm Cortex-M33 Cores (Asymmetric Multi-Processing).
  - **Application Core:** 128 MHz Arm Cortex-M33 with 1 MB Flash + 512 KB RAM, DSP instructions, Hardware Single-Precision FPU (FPv5-SP), Arm TrustZone.
  - **Network Core:** 64 MHz Arm Cortex-M33 with 256 KB Flash + 64 KB RAM dedicated to 2.4 GHz radio processing.
- **CPU Bit Length:** 32-bit (Armv8-M Architecture).
- **Hardware FPU:** **YES** (Hardware Single-Precision IEEE-754 FPU on Application Core).
- **DSP Math Acceleration:** Arm SIMD Audio DSP extensions (single-cycle 32-bit MAC, dual 16-bit MAC, vector operations).
- **Audio Codec Suites:** LC3 (LE Audio / Auracast), LC3plus.
- **Transmit Power:** Up to +3 dBm.
- **Receiver Sensitivity:** -98 dBm (BLE 1 Mbps), -104 dBm (BLE 125 kbps Coded).
- **Digital Audio Interfaces:** 1x Hardware I²S interface (up to 24-bit/192kHz PCM audio), 1x PDM digital mic input, USB 2.0 Full-Speed device.
- **Analog Audio Subsystem:** **NO Onboard Analog DAC** (Requires external I²S DAC like Cirrus Logic CS47L63, PCM5102A, MAX98357A, TAS5805M).

---

## 2. CPU ALU, Hardware FPU & Real-Time DSP Capabilities

Unlike entry-level single-core RISC-V microcontrollers, the nRF5340 incorporates a dedicated **Hardware FPU** and **Arm SIMD DSP instructions** on its 128 MHz Application Core:

```
                                nRF5340 Real-Time Audio DSP Model
                                
  +-----------------------------------------------------------------------------------------+
  |  nRF5340 Application Core (128 MHz Arm Cortex-M33)                                     |
  |  - Hardware Single-Precision FPU (FPv5-SP): Executes `float` math in 1 clock cycle!     |
  |  - Arm SIMD DSP Extensions: Single-cycle 32-bit Multiply-Accumulate (MAC)               |
  |  - Runs real-time `float` Hilbert Transforms, 10-band PEQs & Audio Algorithms seamlessly|
  +-----------------------------------------------------------------------------------------+
                                              ▲
                                    IPC Dual-Core Mailbox
                                              ▼
  +-----------------------------------------------------------------------------------------+
  |  nRF5340 Network Core (64 MHz Arm Cortex-M33)                                          |
  |  - Operates in complete hardware isolation                                              |
  |  - Dedicated exclusively to 2.4 GHz Bluetooth 5.4 Radio ISRs & Link-Layer ISOC Packet   |
  |    Scheduling (BIG/BIS)                                                                 |
  +-----------------------------------------------------------------------------------------+
```

### Key Engineering Advantages of the nRF5340 DSP Architecture:

1. **Hardware Single-Precision FPU (FPv5-SP):**  
   Developers can write complex DSP algorithms (Hilbert 90° phase shifters, biquad parametric EQ crossovers, room acoustic correction, dynamic range compressors) directly using standard 32-bit floating-point `float` C/C++ code. Operations execute in **1 single clock cycle**, avoiding software `soft-fp` emulation penalties.
2. **Zero Core Contention (Asymmetric Dual-Core Isolation):**  
   - **Network Core (64 MHz):** Runs the Bluetooth Link-Layer stack, 2.4 GHz radio ISRs, and microsecond ISOC anchor timing.
   - **Application Core (128 MHz):** Runs the Zephyr RTOS application, LC3 audio decoding, floating-point Hilbert/EQ DSP, and I²S DMA transfers in complete hardware isolation.  
   - **Result:** Bluetooth radio bursts **never interfere or delay real-time audio DSP execution**, guaranteeing zero audio dropouts or buffer underruns!

---

## 3. Bluetooth LE Audio & Auracast Isochronous Channels (ISOC / BIS / BIG)

The nRF5340 Network Core radio baseband contains native hardware support for **Bluetooth 5.4 Isochronous Channels**:

- **Broadcast Isochronous Streams (BIS):** Transmits or receives unidirectional, microsecond-synchronized mono or interleaved stereo LC3 audio channels over the air.
- **Broadcast Isochronous Group (BIG):** Bundles multiple BIS streams with deterministic link-layer anchor timing (<10 µs inter-receiver phase synchronization).
- **Supported LE Audio Profiles:** Public Broadcast Profile (PBP / Auracast), Broadcast Audio Scan Service (BASS), Volume Control Service (VCS), Volume Offset Control Service (VOCS), Published Audio Capabilities Service (PACS).

---

## 4. Software Ecosystem: Zephyr RTOS & nRF Connect SDK (`nrf5340_audio`)

The nRF5340 software ecosystem is built on **Zephyr RTOS** and Nordic's open-source **nRF Connect SDK for Audio**:

```
+--------------------------------------------------------------------------------+
|                   nRF Connect SDK Audio Architecture (`nrf5340_audio`)         |
|                                                                                |
|  +--------------------------------------------------------------------------+  |
|  | Application Layer: Auracast Broadcaster / Receiver / Headset Logic      |  |
|  +--------------------------------------------------------------------------+  |
|  | LE Audio Profile Stack: PBP (Auracast), BASS, VCS, VOCS, PACS              |  |
|  +--------------------------------------------------------------------------+  |
|  | Audio Pipeline: Open-Source LC3 Encoder / Decoder (Zephyr C/C++ Codec)    |  |
|  +--------------------------------------------------------------------------+  |
|  | Kernel: Zephyr RTOS (Preemptive Multi-threading, IPC, Hardware I²S Driver)|  |
|  +--------------------------------------------------------------------------+  |
|                                                                                |
|  * 100% Open-Source: No Qualcomm NDAs, no proprietary binary blobs, no ADK fees|
+--------------------------------------------------------------------------------+
```

- **Open-Source Freedom:** Complete source code visibility down to the Link-Layer and LC3 codec framework.
- **Dual-Core Inter-Processor Communication (IPC):** Uses shared RAM mailboxes and hardware IPC interrupts to exchange HCI ISO audio buffers between the Network Core and Application Core.

---

## 5. Hardware Digital Audio Interfaces (I²S / PDM / USB)

Because the nRF5340 is a digital wireless SoC, it does not include an onboard analog DAC. It provides rich digital interfaces:

1. **Hardware I²S Interface (Master/Slave Mode):**
   - Supports `BCLK` (Bit Clock), `LRCK/WS` (Word Select), `SDOUT` (Data Out), `SDIN` (Data In), and `MCK` (Master Clock Output).
   - Connects directly to external I²S DACs (e.g. PCM5102A, CS47L63, ES9023) or I²S Class-D amplifiers (TAS5805M, MAX98357A).
2. **PDM (Pulse Density Modulation):**
   - Dedicated PDM controller for connecting digital MEMS microphones.
3. **USB 2.0 Full-Speed Controller:**
   - Acts as a USB Audio Class (UAC) device when connected to a PC or smartphone.

---

## 6. Development Hardware & Sourcing Guide

| Hardware Option | Vendor / Source | Form Factor | Key Features & Onboard Hardware | Price |
| :--- | :--- | :--- | :--- | :--- |
| **NRF5340-AUDIO-DK** | Nordic Official | Official Reference DK Board | Includes nRF5340 SoC, Cirrus Logic CS47L63 Smart Codec, USB Audio, 3.5mm line in/out, onboard mics, SEGGER J-Link debugger. | ~170 EUR |
| **MinewSemi MS45SF1** | MinewSemi / AliExpress | Castellated SMT Module | Compact nRF5340 module breaking out I²S, SWD, UART, and USB pins. | ~8 – 12 EUR |
| **AliExpress nRF5340 Dev Board**| AliExpress | DIP Breadboard Dev Board | Low-cost breakout board based on nRF5340 module with 2.54mm headers. | ~10 – 15 EUR |

---

## 7. Pros & Cons Summary for Audio System Designers

### Pros:
- **100% Open-Source C/C++ Ecosystem:** Built on Zephyr RTOS and nRF Connect SDK. No NDA barriers or proprietary toolchain locks.
- **Hardware Single-Precision FPU:** Native 32-bit floating-point math (`float`) for Hilbert transforms, multi-band PEQs, and room correction filters.
- **Zero Core Contention:** Dual-core hardware isolation keeps radio ISRs on the Network Core and audio DSP execution on the Application Core.
- **Native Bluetooth 5.4 Auracast:** Full hardware ISOC (BIS/BIG) support.

### Cons:
- **No Onboard Analog DAC:** Requires an external I²S DAC IC (e.g. PCM5102A, ~$2 USD) for analog audio output.
- **Steeper Software Learning Curve:** Requires familiarity with Zephyr RTOS, CMake, and the Nordic `west` build tool.
- **No Proprietary Codecs:** Supports standard LC3 and LC3plus, but lacks Qualcomm's proprietary aptX Lossless or Sony's LDAC.


# References
* [Nordic nRF5340 YouTube playlist](https://www.youtube.com/playlist?list=PLIDBtYMzw3Jw&si=Y9Q6rF9l1KSEt60U)
* [nRF Connect for VS Code tutorials (YouTube)](https://www.youtube.com/playlist?list=PLx_tBuQ_KSqEt7NK-H7Lu78lT2OijwIMl)
* [Nordic nRF5340 page](https://www.nordicsemi.com/Products/nRF5340)
* [Nordic nRF5340 Development Kit page](https://docs.nordicsemi.com/r/bundle/additionalresources/page/additionalresources/nrf53-series/nrf5340)

---

## 8. Next-Generation Nordic SoCs: nRF54 Series Migration Path

Nordic Semiconductor has announced the **nRF54 Series**, succeeding both the nRF53 and nRF52 product lines on an advanced **22 nm process node**:

1. **nRF54H20 (High-Performance Flagship):**
   - Dual Arm Cortex-M33 (Application Core up to **320 MHz** + Network Core @ **256 MHz**) + dedicated **128 MHz RISC-V coprocessors**.
   - **2.0 MB NVM** + **1.0 MB (1024 KB) SRAM**.
   - High-Speed USB 2.0 (480 Mbps), CAN-FD, I3C, 14-bit ADC, **+10 dBm TX power**, **-100 dBm RX sensitivity**.

2. **nRF54LM20A (Premium Ultra-Low-Power):**
   - **128 MHz Arm Cortex-M33** + **128 MHz RISC-V coprocessor**, **2.0 MB NVM**, **512 KB SRAM**, High-Speed USB 2.0.

3. **nRF54L Series (nRF54L15 / nRF54L10 / nRF54L05):**
   - 22 nm ultra-low-power architecture with 128 MHz Cortex-M33 + 128 MHz RISC-V coprocessor.
   - Scalable RRAM / RAM options: **nRF54L15** (1.5 MB RRAM / 256 KB RAM), **nRF54L10** (1.0 MB RRAM / 192 KB RAM), **nRF54L05** (0.5 MB RRAM / 96 KB RAM).
   - Full hardware ISOC / BIS Auracast support, I²S digital audio, and PSA Level 3 security.

4. **Legacy nRF52 Warning (nRF52840 / nRF52833):**
   - Standard nRF52 series chips **lack hardware Isochronous Baseband Channels (ISOC/BIS)** in silicon and **cannot** run standard Bluetooth SIG Auracast natively. All modern LE Audio designs must target nRF5340 or the nRF54 series.

