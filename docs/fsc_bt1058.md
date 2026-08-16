# Feasycom FSC-BT1058 (Qualcomm QCC5181) Hardware & Communications Specification

This document details the hardware specifications, electrical characteristics, MCU connection topology, and UART/GATT communications interface for the **Feasycom FSC-BT1058** Bluetooth 5.4 Dual-Mode and LE Audio module.

Original Datasheet Reference: [Feasycom FSC-BT1058.pdf](file:///c:/Git_forestChirp/auracast_BT5.2/Feasycom%20FSC-BT1058.pdf)  
Online User Guide: [Feasycom BT1058 User Guide](https://document.feasycom.com/docs/fsc-audio/EN/latest/bt1058/index.html)  
Online API Documentation: [Feasycom BT1058 Documentation](https://document.feasycom.com/docs/audio/BT1058_EN/latest/)

---

## 1. Module Overview & Key Specifications

The **FSC-BT1058** is an industrial-grade castellated surface-mount Bluetooth module powered by the **Qualcomm QCC5181** System-on-Chip (SoC).

```
+--------------------------------------------------------------------------------+
|                            Feasycom FSC-BT1058                                 |
|                                                                                |
|  +-----------------------+     +-------------------+     +------------------+  |
|  | Dual 240 MHz Kalimba  |     |  Qualcomm QCC5181 |     | 24-bit Audio     |  |
|  | Audio DSPs            |     |  BT 5.4 LE Audio  |     | Codec & I²S/TDM  |  |
|  +-----------------------+     +-------------------+     +------------------+  |
|                                                                                |
|  * Form Factor: 52-Pin Castellated SMT (13mm x 26.9mm x 2.2mm, 1.0mm Pitch)    |
|  * Embedded Firmware: Feasycom Transparent UART AT Command Stack              |
+--------------------------------------------------------------------------------+
```

### Key Technical Parameters

- **Bluetooth Version:** Bluetooth Core Specification 5.4 (Dual-Mode BR/EDR + LE Audio).
- **Core Processor:** Dual 240 MHz Qualcomm Kalimba 32-bit DSP cores + 32/80 MHz Developer Processor.
- **Audio Codec Suites:** LC3 (LE Audio / Auracast), aptX, aptX HD, aptX Adaptive, aptX Lossless (24-bit/96kHz), LDAC, SBC, AAC.
- **Transmit Power:** +15 dBm Maximum.
- **Receiver Sensitivity:** -100.5 dBm (BLE 1 Mbps), -96.5 dBm (π/4 DQPSK), -90.0 dBm (8DPSK).
- **Digital Audio Interfaces:** 24-bit I²S/PCM (1 input, 3 output channels up to 384 kHz), 8-slot TDM, S/PDIF (2 instances), Audio MCLK output.
- **Analog Audio Subsystem:** Stereo 24-bit HQADC (Line/Mic inputs), Stereo 24-bit HQDAC (Line/Headphone out), 10x Digital Microphone inputs.
- **Host Communications Interface:** UART (TX, RX, CTS, RTS) with baud rates from 2400 to 4,000,000 baud (default `115200, N, 8, 1`).

### Feasycom Module Comparison Table: FSC-BT1058 vs. FSC-BT1038A vs. FSC-BT1038B

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

---

## 2. Electrical Operating Characteristics

| Parameter | Symbol | Min | Typical | Max | Unit | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Main Battery Input** | `VBAT_IN` | 2.80 | 3.30 / 3.70 | 4.30 | V | Primary power rail (Pin 33) |
| **I/O Reference Voltage** | `VDD_IO` | 1.70 | 1.80 / 3.30 | 3.60 | V | CMOS logic reference (Pin 36). Connect to ESP32-S3 3.3V rail. |
| **USB / Charger Input** | `VCHG` / `VBUS`| 4.75 | 5.00 | 5.75 / 6.50| V | USB 5V supply input (Pin 39). Optional if battery/3.3V powered. |
| **Power Control Input** | `SYS_CTRL` | 0.00 | 3.30 | 4.25 | V | Power-on trigger input (Pin 34) |
| **Audio Output Power** | `VOUT` | — | 1000 | — | mV | 0dBFS into 10k load |
| **Operating Temperature** | `TA` | -40 | +20 | +85 | °C | Industrial grade |

---

## 3. FSC-BT1058 Complete Pinout & ESP32-S3 Connection Guide

The FSC-BT1058 features 52 castellated pads. Below is the pinout mapping and the recommended connection guide for interfacing with an **ESP32-S3** host MCU.

### Complete 52-Pad Pinout Table

| Pad # | Pin Name | Type | Description | ESP32-S3 Connection & Requirement |
| :--- | :--- | :--- | :--- | :--- |
| **1** | `GND` | Vss | Power Ground | System Ground Plane |
| **2** | `AIO3/LED3` | A, I/O | General Purpose Analog/Digital I/O or LED | NC or Status LED |
| **3** | `NC` | NC | Do Not Connect | Leave Floating (Do Not Ground) |
| **4** | `PCM_CLK / PIO16` | I/O | I²S / PCM Bit Clock (`BCLK`) | **ESP32-S3 `I2S_BCLK` GPIO** |
| **5** | `PCM_IN / PIO19` | I/O | I²S / PCM Serial Data Input (`DIN`) | **ESP32-S3 `I2S_DOUT` GPIO** (Transmitter Mode) |
| **6** | `PCM_OUT / PIO18` | I/O | I²S / PCM Serial Data Output (`DOUT`) | **ESP32-S3 `I2S_DIN` GPIO** / DAC Data (Receiver Mode) |
| **7** | `PCM_SYNC / PIO17`| I/O | I²S / PCM Frame Sync / Word Select (`LRCK`)| **ESP32-S3 `I2S_WS` GPIO** |
| **8** | `RESET#` | I/O | Active-Low Hardware Reset (Pull-up to VDD_IO)| **ESP32-S3 GPIO** (Pull LOW >120µs for reset) |
| **9** | `PCM_MCLK_OUT` | I/O | Master Clock Output (`MCLK`) | ESP32-S3 / External DAC MCLK Input (Optional) |
| **10**| `PIO6` | I/O | Programmable I/O line 6 | NC / General I/O |
| **11**| `PIO7` | I/O | Programmable I/O line 7 | NC / General I/O |
| **12**| `PIO8` | I/O | Programmable I/O line 8 | NC / General I/O |
| **13**| **`BT_TX / PIO5`** | I/O | Module UART Serial Data Output | **ESP32-S3 `UART_RX` GPIO** (3.3V Logic) |
| **14**| **`BT_RX / PIO4`** | I/O | Module UART Serial Data Input | **ESP32-S3 `UART_TX` GPIO** (3.3V Logic) |
| **15**| **`BT_CTS / PIO3`**| I/O | UART Clear To Send (Active Low) | **ESP32-S3 `RTS` GPIO** (Strongly Recommended) |
| **16**| **`BT_RTS / PIO2`**| I/O | UART Request To Send (Active Low) | **ESP32-S3 `CTS` GPIO** (Strongly Recommended) |
| **17**| `LED0 / AIO0` | A, I/O | LED Output 0 (Pairing Indicator) | Status LED / NC |
| **18**| `LED1 / AIO1` | A, I/O | LED Output 1 (Connection Indicator) | Status LED / NC |
| **19**| `LED2 / AIO2` | A, I/O | LED Output 2 | Status LED / NC |
| **20**| `PIO34` | I/O | Programmable I/O line 34 | NC |
| **21**| `PIO35` | I/O | Programmable I/O line 35 | NC |
| **22**| `GND` | Vss | Power Ground | System Ground Plane |
| **23**| `PIO36` | I/O | Programmable I/O line 36 | NC |
| **24**| `PIO37` | I/O | Programmable I/O line 37 | NC |
| **25**| `PIO38` | I/O | Programmable I/O line 38 | NC |
| **26**| `PIO39` | I/O | Programmable I/O line 39 | NC |
| **27**| `PIO20` | I/O | Programmable I/O line 20 | NC / Alternate PCM_DOUT[1] |
| **28**| `PIO21` | I/O | Programmable I/O line 21 | NC / Alternate PCM_DOUT[2] |
| **29**| `VCHG_SENSE` | Input | Charger Sense Input | Connect to VCHG if charger used, else NC |
| **30**| `CHG_EXT` | Output | External Charger Control | NC |
| **31**| `VDD_USB/3.3V_OUT`| Output| 3.3V Output from internal LDO (Max 50mA) | Local Decoupling Cap (100nF) / NC |
| **32**| `GND` | Vss | Power Ground | System Ground Plane |
| **33**| **`VBAT_IN`** | Vdd | Main Power Supply Input (2.8V – 4.3V) | **3.3V System Power Rail** (Decouple 10µF + 100nF) |
| **34**| **`SYS_CTRL`** | Input | **Power-On Regulator Enable Input** | **ESP32-S3 GPIO** (Must pull HIGH >100ms to boot) |
| **35**| `1.8V_OUT` | Output | Internal 1.8V LDO Output (Max 30mA) | Local Decoupling Cap (1uF) / NC |
| **36**| **`VDD_IO`** | Input | PIO Supply Reference (1.7V – 3.6V) | **3.3V System Power Rail** (Same as ESP32-S3) |
| **37**| `USB_DP` | I/O | USB 2.0 Full Speed Data Positive (D+) | USB Connector D+ / NC |
| **38**| `USB_DN` | I/O | USB 2.0 Full Speed Data Negative (D-) | USB Connector D- / NC |
| **39**| `VCHG` | Vdd | Charger / USB 5V Input | 5V Rail / NC |
| **40**| `MIC_RP` | Analog | Microphone 2 / Line-In Right (+) | Analog Audio Input Right (+) / NC |
| **41**| `MIC_RN` | Analog | Microphone 2 / Line-In Right (-) | Analog Audio Input Right (-) / NC |
| **42**| `NC` | NC | Do Not Connect | Leave Floating |
| **43**| `MIC_LP` | Analog | Microphone 1 / Line-In Left (+) | Analog Audio Input Left (+) / NC |
| **44**| `MIC_LN` | Analog | Microphone 1 / Line-In Left (-) | Analog Audio Input Left (-) / NC |
| **45**| `MIC_BIAS` | Output | Electret Microphone Bias Output | Microphone Bias Circuit / NC |
| **46**| `SPK_RN` | Analog | Differential Line/Speaker Out Right (-) | Analog Audio Out Right (-) / NC |
| **47**| `SPK_RP` | Analog | Differential Line/Speaker Out Right (+) | Analog Audio Out Right (+) / NC |
| **48**| `SPK_LN` | Analog | Differential Line/Speaker Out Left (-) | Analog Audio Out Left (-) / NC |
| **49**| `SPK_LP` | Analog | Differential Line/Speaker Out Left (+) | Analog Audio Out Left (+) / NC |
| **50**| `GND` | Vss | Power Ground | System Ground Plane |
| **51**| `RF_OUT` | RF Port| 50-ohm Bluetooth Antenna Port | On-board PCB Antenna or IPEX Connector |
| **52**| `GND` | Vss | Power Ground | System Ground Plane |

---

## 4. Critical Hardware Power-On & Reset Sequence

> [!CAUTION]
> **`SYS_CTRL` (Pin 34) Mandatory Power-On Requirement:**  
> The module will **NOT** turn on simply by applying 3.3V to `VBAT_IN` and `VDD_IO`. `SYS_CTRL` is the power enable signal for the QCC5181 internal SMPS regulators.

```
                  +--------------------------------------------------+
                  |  FSC-BT1058 Power-On / Reset Timing Diagram      |
                  +--------------------------------------------------+

VBAT_IN / VDD_IO  ___/=================================================  (3.3V Power Rail)
                        
SYS_CTRL (Pin 34) ________________/====================================  (High >100ms after power)
                                 |<--- t_BOOT (>100ms) --->|
                                 
RESET# (Pin 8)    =====================================================  (Pulled High to VDD_IO)
```

### Recommended Startup Code Logic (ESP32-S3)
1. Power up `VBAT_IN` (Pin 33) and `VDD_IO` (Pin 36).
2. Keep `SYS_CTRL` (Pin 34) LOW for 20 ms.
3. Drive `SYS_CTRL` HIGH from an ESP32-S3 GPIO for at least **100 ms** to trigger module bootup.
4. (Optional) Drive `RESET#` (Pin 8) LOW for >120 µs to issue a clean hardware reset if necessary.

---

## 5. Host Communications & Interface Options (UART vs. SPI vs. I²C vs. USB)

The FSC-BT1058 provides multiple physical serial communication interfaces on its hardware pads. However, their roles differ significantly in Feasycom's default module firmware:

### Comparison of Physical Communication Interfaces

| Interface | Module Pins | Supported in Feasycom Default FW? | Primary Purpose / Recommended Role |
| :--- | :--- | :--- | :--- |
| **UART Interface** | `BT_TX` (Pin 13), `BT_RX` (Pin 14), `BT_CTS` (Pin 15), `BT_RTS` (Pin 16) | **YES (Default AT Command Interface)** | **Primary Host MCU Control Port:** Executes AT commands (`AT+LEAUDIO...`, `AT+VOL...`) up to 4 Mbaud. |
| **SPI Interface** | `TBR_MOSI[0]` (Pin 10), `TBR_MISO[0]` (Pin 11), `TBR_CLK` (Pin 12) | **NO (Hardware Debug / Flashing Only)** | **Qualcomm TRBI200 SPI Debugger:** Used exclusively to re-flash firmware, edit PSKeys, and debug code. |
| **I²C Interface** | `I2C_SDA` / `I2C_SCL` (Muxed on PIOs) | Master Mode Only (Up to 400 kbps) | Driving external peripherals (external I²C codecs, DACs, OLED displays). Not used for AT control. |
| **USB 2.0 Interface**| `USB_DP` (Pin 37), `USB_DN` (Pin 38) | YES (Virtual COM Port + USB Audio Class) | PC/Mac AT command control & USB soundcard interface. |

> [!IMPORTANT]
> **Why UART is Recommended over SPI for Host MCU Control:**  
> While the QCC5181 SoC has physical SPI pins (`TBR_MOSI`, `TBR_MISO`, `TBR_CLK`), Qualcomm's hardware architecture assigns this SPI port to the **Transaction Bridge (TRBI200) Debug Subsystem**. Feasycom's transparent firmware does **NOT** run an AT command parser on the SPI slave interface.  
>   
> **Hardware Recommendation:** Use **UART with RTS/CTS flow control** for ESP32-S3 runtime control, and break out the **SPI pins to a 4-pin test header** for initial firmware flashing / PSKey configuration.

### Hardware UART Connection Schematic

```
+------------------------------------+          +------------------------------------+
|     FSC-BT1058 (QCC5181)           |          |      ESP32-S3 Microcontroller      |
|                                    |          |                                    |
|  Pin 13 (BT_TX) -------------------+--------> | GPIO (UART_RX)                     |
|  Pin 14 (BT_RX) <------------------+--------- | GPIO (UART_TX)                     |
|  Pin 15 (BT_CTS) <-----------------+--------- | GPIO (UART_RTS) (Active Low)       |
|  Pin 16 (BT_RTS) ------------------+--------> | GPIO (UART_CTS) (Active Low)       |
|                                    |          |                                    |
|  Pin 34 (SYS_CTRL) <---------------+--------- | GPIO (Power Enable)                |
|  Pin 8  (RESET#) <-----------------+--------- | GPIO (Hardware Reset)              |
+------------------------------------+          +------------------------------------+
```

### Why Hardware RTS/CTS Flow Control is Recommended

In Bluetooth LE Audio systems, hardware flow control via `BT_RTS` (Pin 16) and `BT_CTS` (Pin 15) is strongly recommended over floating or bypassed UART wiring for three reasons:

1. **Datasheet Buffer Overrun & Processor Crash Warning (Page 17):**
   - Feasycom explicitly warns that bypassing CTS/RTS when transmitting high-speed UART data risks **overflowing internal receive buffers**. On the QCC5181, a UART buffer overflow triggers a **processor crash / stack deadlock**, dropping active Bluetooth connections and requiring a full hardware power cycle (`SYS_CTRL` reset).
2. **RF Task Burst Buffering:**
   - During heavy Bluetooth LE Audio operations (e.g. Periodic Advertising encoding, LC3 frame processing, or handling incoming GATT connection requests), the module's CPU priority shifts to high-speed RF tasks.
   - When the module's internal UART RX FIFO hits its high-water mark, it deasserts `BT_RTS` (pulls it HIGH). The ESP32-S3 hardware UART controller instantly pauses transmission at the hardware logic level without losing data bytes or stalling CPU execution loops.
3. **Enabling High Baud Rates (921,600 baud to 4 Mbaud):**
   - Transmitting large GATT payloads (like custom biquad PEQ filter matrices or room correction curves) requires high baud rates.
   - At baud rates above 115,200, software-based flow control (XON/XOFF) is too slow. Hardware RTS/CTS operate at nanosecond logic line speed, guaranteeing zero packet corruption at 921.6 kbps+.

---

### Essential LE Audio / Auracast AT Command Reference

| Function / Category | AT Command Syntax | Description & Expected Response |
| :--- | :--- | :--- |
| **Test Command** | `AT` | Module responds with `OK`. |
| **Baud Rate Config** | `AT+BAUD=115200` | Sets UART baud rate (options: 9600, 19200, 115200, 921600, 4000000). |
| **Device Name** | `AT+NAME=4.2_Master_Hub` | Sets local Bluetooth device broadcast name. |
| **LE Audio Role** | `AT+LEAUDIO=1` | Sets LE Audio role (`0` = Off, `1` = Broadcaster/Source, `2` = Sink/Receiver). |
| **Auracast Broadcast**| `AT+LEAUDIO=1,1,"Auracast_4.2"` | Starts BIG broadcast stream with broadcast name `"Auracast_4.2"`. |
| **BASS Scan (Sink)** | `AT+BASS=1` | Instructs receiver node to scan for Periodic Advertising (PA) streams. |
| **Select BIS Stream** | `AT+BISSELECT=1` | Syncs receiver node to specific BIS channel index (`1` to `31`). |
| **VCS Master Volume** | `AT+VOL=180` | Sets VCS Master Volume (`0` to `255`). |
| **VOCS Channel Offset**| `AT+VOCS=0,-300` | Sets VOCS gain offset on channel 0 (`-300` = `-3.00 dB` trim for subs/satellites). |
| **GATT Custom Write** | `AT+GATTWRITE=...` | Writes custom LTV metadata (e.g., PEQ biquads) to GATT server. |

---

## 6. Digital Audio (I²S / PCM) Configuration

The FSC-BT1058 provides a 24-bit I²S digital audio bus capable of sample rates from **8 kHz up to 384 kHz**.

### Master vs. Slave Clocking Topologies

- **Receiver Node (FSC-BT1058 as I²S Master):**
  - FSC-BT1058 generates `PCM_CLK` (`BCLK`) and `PCM_SYNC` (`LRCK`).
  - Decoded LC3 audio flows out of `PCM_OUT` (Pin 6) into the ESP32-S3 `I2S_DIN` or directly into an external I²S DAC (e.g. PCM5102A / TAS5805M).
  - ESP32-S3 I²S peripheral is configured in **I²S Slave Mode**.

- **Transmitter Master Hub Node (FSC-BT1058 as I²S Slave):**
  - ESP32-S3 acts as **I²S Master**, generating `BCLK` and `LRCK`.
  - ESP32-S3 outputs mixed/filtered 24-bit/48kHz stereo audio from its `I2S_DOUT` pin into the FSC-BT1058's `PCM_IN` (Pin 5).
  - FSC-BT1058 encodes incoming I²S audio to LC3 frames and broadcasts them over Auracast BIS channels.

---

## 6.5 On-board Analog HQDAC & Balanced XLR Output Buffer Circuit

The FSC-BT1058 includes an internal 24-bit High-Quality stereo DAC (**HQDAC**) with true differential analog outputs available directly on castellated pins:

- **Right Channel Differential Pair:** `SPK_RP` (Pin 47) and `SPK_RN` (Pin 46)
- **Left Channel Differential Pair:** `SPK_LP` (Pin 49) and `SPK_LN` (Pin 48)
- **HQDAC Performance Specs:** 103.8 dBA SNR, -92.5 dB THD+N, 1000 mV RMS (0 dBFS into 10k load).

### Driving Professional Balanced XLR Power Amplifiers

To connect the FSC-BT1058 differential outputs directly to professional power amplifiers or studio monitors expecting **balanced XLR inputs (+4 dBu nominal)**, an active op-amp buffer stage is recommended for three reasons:

1. **DC Blocking:** The QCC5181 analog outputs carry an internal DC common-mode bias voltage (~1.5V VAG reference). AC-coupling capacitors (2.2µF to 10µF) must be placed in series on both positive and negative differential lines.
2. **Signal Level Gain Matching:** The module outputs ~1.0V RMS (Consumer 0 dBFS). Professional studio amplifiers expecting +4 dBu (1.228V RMS nominal, ~4V to 6V peak-to-peak headroom) benefit from a low-noise differential buffer stage with a clean gain of ~1.2x to 2.0x.
3. **Low Impedance Line Driving:** Driving long, balanced XLR cable runs (5m to 20m) requires low output impedance (<50 ohms). An active op-amp buffer isolates the QCC5181 internal DAC from capacitive cable loading.

```
[ FSC-BT1058 Module ]                    [ Op-Amp Buffer Stage (OPA1612 / NE5532) ]              [ Pro XLR Output ]
Pin 49 (SPK_LP) ───[ 2.2uF Cap ]───(+)───> \                                                    ───> Pin 2 (Hot)
                                           | Op-Amp Differential Buffer (Gain ~1.2x) | ───>
Pin 48 (SPK_LN) ───[ 2.2uF Cap ]───(-)───> /                                                    ───> Pin 3 (Cold)

GND (System Ground) ────────────────────────────────────────────────----------------────────────> Pin 1 (Shield)
```

---

## 6.6 Standalone Bi-Amped 2-Way Active Crossover Topology (Tweeter + Mid-Bass Driver)

A single FSC-BT1058 module can act as a **complete active digital crossover engine for a 2-way satellite speaker (Tweeter + Mid-Bass)** without an external MCU or passive crossover component network:

```
                                      FSC-BT1058 Standalone Module (QCC5181)
                           +----------------------------------------------------------+
                           |                                                          |
                           |   Incoming BIS Mono Audio Payload (LC3 Decoded Stream)   |
                           |                            │                             |
                           |         ┌──────────────────┴──────────────────┐          |
                           |         ▼                                     ▼          |
                           |  Kalimba DSP Chain 0                  Kalimba DSP Chain 1 |
                           |  (High-Pass Filter fc=3.0kHz)         (Band-Pass 80Hz-3kHz)|
                           |  (Tweeter Delay Alignment)            (Woofer Baffle PEQ)  |
                           |         │                                     │          |
                           |         ▼                                     ▼          |
                           |   Left DAC (`SPK_LP`/`SPK_LN`)          Right DAC (`SPK_RP`/`SPK_RN`)
                           +---------│-------------------------------------│----------+
                                     │                                     │
                                     ▼                                     ▼
                           [ Tweeter Power Amp ]                 [ Mid-Bass Power Amp ]
                                     │                                     │
                                     ▼                                     ▼
                              [ Tweeter Driver ]                    [ Mid-Bass Driver ]
```

### Key Engineering Advantages of Module Bi-Amping:
1. **Eliminates Passive Crossovers:** Eliminates bulky passive capacitors and inductors that cause phase distortion and power loss inside speaker enclosures.
2. **Sharp 24 dB/Octave Digital Filters:** The Kalimba DSP runs Linkwitz-Riley / Butterworth 4th-order active digital crossover filters natively.
3. **Time Alignment / Delay per Driver:** Adjusts acoustic center delays (e.g. 50–150 µs delay on the tweeter) so sound from both voice coils reaches the listener's ear simultaneously.
4. **Independent Channel PEQ:** Feasycom AT commands (`AT+PEQ=0,...` for Left/Tweeter, `AT+PEQ=1,...` for Right/Woofer) allow independent gain and parametric EQ tuning per driver.

---

## 7. PCB Layout & Hardware Integration Recommendations

1. **Antenna Clearance Zone:**
   - Leave a **5mm x 10mm copper clearance area** underneath and around the PCB antenna (Pin 51 `RF_OUT`). No ground planes, signal traces, or metal enclosures should be placed in this restricted zone.
2. **Decoupling Capacitors:**
   - Place a **10µF bulk capacitor in parallel with a 100nF ceramic capacitor** as close as possible to `VBAT_IN` (Pin 33) and `VDD_IO` (Pin 36).
3. **Grounding:**
   - Use a solid, uninterrupted Ground plane beneath the module. Connect all GND pads (Pins 1, 22, 32, 50, 52) to the main ground plane using multiple ground vias.
4. **UART Trace Routing:**
   - Keep `BT_TX` and `BT_RX` traces short and route them away from high-frequency clock lines (`PCM_CLK`, `PCM_MCLK_OUT`) to prevent crosstalk.

---

## 8. Summary Checklist for ESP32-S3 + FSC-BT1058 Hardware Build

- [x] Connect `VBAT_IN` (Pin 33) and `VDD_IO` (Pin 36) to 3.3V rail.
- [x] Connect `SYS_CTRL` (Pin 34) to an ESP32-S3 GPIO for hardware boot activation.
- [x] Connect `BT_TX` (Pin 13) and `BT_RX` (Pin 14) to ESP32-S3 UART pins with `BT_RTS`/`BT_CTS` flow control.
- [x] Route `PCM_CLK` (Pin 4), `PCM_SYNC` (Pin 7), `PCM_OUT` (Pin 6), and `PCM_IN` (Pin 5) for 24-bit I²S digital audio data flow.
- [x] Maintain 5mm RF antenna clearance zone on PCB.

---

## 9. Transmitter Hub Architecture: Division of Labor (ESP32-S3 vs. FSC-BT1058)

In this streamlined 4.2 Auracast network architecture, **all 6 receiver speaker nodes run completely standalone** (FSC-BT1058 modules pre-configured with flash-stored Kalimba bi-amp/sub crossovers). 

Because the receiver nodes operate autonomously, **no wireless control mesh (ESP-NOW / Wi-Fi UDP) or slave receiver MCUs are required**. The Master ESP32-S3 focuses exclusively on source switching, volume scaling/normalization, and master EQ.

```
[ Audio Input Sources ]
 ├── Analog AUX (Line-In ADC) ──────────────┐
 ├── Wi-Fi Stream (AirPlay 2 / Spotify) ────┼──> [ ESP32-S3 Master Hub ] ──(I²S DOUT)──> [ FSC-BT1058 Transmitter ]
 ├── USB Audio Class 2.0 -------------------┤    - Multi-Source Switching                 - LC3 Broadcast Encoder
 └── Legacy BT A2DP (Phone SBC/AAC) --------┘    - Gain Normalization & Fading            - BIG / BIS Packet Scheduler
                                                 - Master Parametric EQ                   - Periodic Advertising (PA)
                                                 - Web Server / OLED UI                   - Auracast Broadcast Manager
```

---

### 1. Functional Division of Labor Matrix (Streamlined Architecture)

| Task / Feature | Handled By | Why it belongs on this processor |
| :--- | :--- | :--- |
| **Multi-Source Audio Switching** | **ESP32-S3** | Manages inputs (Wi-Fi streaming, AUX line-in, USB audio, A2DP) and cross-fades smoothly between active sources. |
| **Gain Normalization & Level Scaling**| **ESP32-S3** | Normalizes input volume levels across loud/quiet sources before feeding the transmitter. |
| **Master Parametric EQ (Room Curve)** | **ESP32-S3** | Applies optional master system-wide EQ tone controls / room curve to the 24-bit I²S PCM stream. |
| **Web Server, OLED UI & User Controls**| **ESP32-S3** | Handles local user controls, web interface, physical buttons/rotary knobs, and display. |
| **LC3 Audio Frame Encoding** | **FSC-BT1058** | QCC5181 hardware DSP compresses raw I²S PCM stereo audio into high-efficiency LC3 frames in real time. |
| **BIG / BIS Link-Layer Scheduling**| **FSC-BT1058** | QCC5181 hardware Bluetooth MAC manages sub-10 µs microsecond isochronous frame anchor timing. |
| **Periodic Advertising (PA)** | **FSC-BT1058** | FSC-BT1058 broadcasts `BIGInfo`, stream names, encryption keys, and metadata trains over 2.4 GHz RF. |
| **Crossover Filtering (LP / HP)** | **Receiver Modules**| Pre-configured flash-stored Kalimba DSP filters inside receiver BT1058 modules (no master transmitter crossover required!). |

---

### 2. Master Hub Setup AT Command Initialization Sequence

When the Master Hub powers up, the ESP32-S3 initializes the FSC-BT1058 transmitter via the following UART AT command sequence:

```
1. ESP32-S3 drives SYS_CTRL (Pin 34) HIGH for >100ms to boot QCC5181.
2. Send: AT                            ---> Response: OK
3. Send: AT+BAUD=115200                ---> Response: OK
4. Send: AT+NAME=4.2_Master_Hub        ---> Response: OK
5. Send: AT+LEAUDIO=1                  ---> Response: OK  (Sets Broadcaster/Transmitter Mode)
6. Send: AT+AUDIOIN=I2S                ---> Response: OK  (Selects PCM_IN I2S Digital Input)
7. Send: AT+CODEC=LC3,48000,16,10      ---> Response: OK  (48 kHz, 16-bit, 10ms LC3 frames)
8. Send: AT+BISNUM=2                   ---> Response: OK  (Configures 2 BIS streams inside BIG for Stereo L/R)
9. Send: AT+LEAUDIO=1,1,"Auracast_4.2" ---> Response: OK  (Starts BIG Broadcast Stream)
```

---

## 10. Physical Module Prototyping & Soldering Solutions (1.0 mm Pad Pitch)

The FSC-BT1058 features **1.0 mm pad pitch castellated edges**, which is significantly finer than standard 2.54 mm (0.1") pin headers. Direct hand-soldering of standard jumper wires to adjacent 1.0 mm pads can cause solder bridges and mechanical pad damage.

### Recommended Physical Interconnection Methods

#### 1. Official Feasycom FSC-DB105 / FSC-DB200 Development Board (Easiest)
- **Description:** Official carrier breakout board from Feasycom with the FSC-BT1058 module pre-soldered.
- **Features:** Breaks out all module pins to standard **2.54 mm (0.1") breadboard-friendly DIP headers**. Includes USB-to-UART converter chip, 3.3V LDO, 3.5mm headphone jacks, and status LEDs.
- **Sourcing:** Available directly on Feasycom / AliExpress (~15 to 22 EUR).

#### 2. Cheap 1.0 mm SMT-to-2.54 mm DIP Breakout PCB Adapter (~1–2 EUR)
- **Description:** Generic 2-layer PCB breakout board designed for 1.0 mm pitch castellated modules.
- **Implementation:** Solder the bare FSC-BT1058 module onto the 1.0 mm pad footprint in the center, and solder standard 2.54 mm male pin headers to the outer edge.
- **Custom PCB Alternative:** Draw a minimal 20mm x 30mm KiCAD adapter board breaking out only the required 10 pins (`VBAT`, `VDD_IO`, `GND`, `SYS_CTRL`, `BT_TX`, `BT_RX`, `BT_CTS`, `BT_RTS`, `PCM_CLK`, `PCM_SYNC`, `PCM_OUT`/`IN`). Order 5 PCBs from JLCPCB for $2.00.

#### 3. Direct Hand-Soldering with 30 AWG Wire + Hot Glue Strain Relief (Immediate Bench Testing)
- **Procedure:**
  1. Apply liquid rosin flux across the castellated pads.
  2. Pre-tin only the required pads (`VBAT`, `GND`, `SYS_CTRL`, `TX`, `RX`, I²S pins).
  3. Strip ~1mm of **30 AWG Kynar wire** or **0.1mm enamelled copper magnet wire**.
  4. Solder thin 30 AWG wires into the crescent edge of each pad.
  5. Solder the opposite wire ends to a standard 2.54 mm header board.
  6. **CRITICAL:** Apply a generous dab of **Hot Glue** or UV conformal epoxy over the soldered wire connections on the FSC-BT1058 to act as mechanical strain relief, preventing wire tugs from tearing the 1.0 mm pads off the PCB.


