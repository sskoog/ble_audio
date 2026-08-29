#!/usr/bin/env python3
"""
Real-Time Windows 11 PC Audio Streamer for audioESP-NOW (VSAF Protocol)
-----------------------------------------------------------------------
Streams real-time PC audio (MP3 Folder Playlist / WASAPI Loopback / Synthesizer) 
to the ESP-NOW SOURCE Node (Node 21 on COM121) over high-speed USB Serial.

Features:
- Seamless random MP3 playback from data/mp3 folder (default).
- High-fidelity polyphase resampling to any VSAF sample rate (8k..48k).
- Support for both 7.5 ms and 10.0 ms LC3 frame durations.
- Multi-channel support (1 to 6 discrete audio channels).
- Google liblc3 C-accelerated real-time LC3 encoder array.
- 248-byte word-aligned VSAF packet serialization with dual-frame redundancy.
"""

import sys
import os
import time
import struct
import math
import argparse
import collections
import numpy as np
from fractions import Fraction

# Add apps/usb_ble_bumble to python path to access lc3_encoder and audio_source
CURRENT_DIR = os.path.dirname(os.path.abspath(__file__))
BUMBLE_DIR = os.path.join(CURRENT_DIR, "..", "usb_ble_bumble")
if os.path.exists(BUMBLE_DIR):
    sys.path.insert(0, os.path.abspath(BUMBLE_DIR))

try:
    from lc3_encoder import LC3Encoder
    from audio_source import Mp3FolderAudioSource, LfoSineAudioSource
except ImportError:
    from apps.usb_ble_bumble.lc3_encoder import LC3Encoder
    from apps.usb_ble_bumble.audio_source import Mp3FolderAudioSource, LfoSineAudioSource

import serial
import serial.tools.list_ports
from scipy import signal

# VSAF Protocol Constants
VSAF_MAGIC = 0x1337
VSAF_HEADER_LEN = 8

SAMPLE_RATE_CODES = {
    8000: 0,
    16000: 1,
    24000: 2,
    32000: 3,
    44100: 4,
    48000: 5
}

# Pentatonic Frequencies for 6 distinct channels
PENTATONIC_CH_FREQS = [220.0, 330.0, 440.0, 550.0, 660.0, 880.0]


def auto_detect_source_port() -> str:
    """Auto-detects the UART / CH343 COM port for Node 21 SOURCE (COM121)."""
    try:
        ports = list(serial.tools.list_ports.comports())
        # Check for CH343 / CH340 / CP210x UART chip first (Node 21 UART / COM121)
        for p in ports:
            if "1A86:55D3" in p.hwid or "CH34" in p.description or "COM121" in p.device:
                return p.device
        # Next check for COM21 / ESP32-C6 USB Serial
        for p in ports:
            if "303A:1001" in p.hwid or "COM21" in p.device:
                return p.device
    except Exception:
        pass
    return "COM121"


def get_default_octets(sample_rate: int, duration_us: int) -> int:
    """Returns recommended LC3 octets per frame (120 octets = 128 kbps @ 7.5ms, 96 kbps @ 10ms for max fidelity)."""
    return 120


def calculate_required_samples(sample_rate: int, duration_us: int) -> int:
    if duration_us == 7500:
        if sample_rate == 44100: return 330
        return int(sample_rate * 75 / 10000)
    else:
        if sample_rate == 44100: return 441
        return int(sample_rate * 10 / 1000)


class MultiChannelResampler:
    """Polyphase bandlimited resampler for multi-channel streaming."""
    def __init__(self, in_rate: int, out_rate: int):
        self.in_rate = in_rate
        self.out_rate = out_rate
        frac = Fraction(out_rate, in_rate).limit_denominator(100)
        self.up = frac.numerator
        self.down = frac.denominator

    def resample(self, pcm_data: np.ndarray) -> np.ndarray:
        if self.in_rate == self.out_rate:
            return pcm_data
        return signal.resample_poly(pcm_data, self.up, self.down, axis=0)


class PcAudioStreamer:
    def __init__(self, port: str, baud: int, sample_rate: int, duration_ms: float,
                 num_channels: int, octets: int, source_type: str, mp3_dir: str = None, audio_device: str = None):
        self.port = port if port and port.upper() != "AUTO" else auto_detect_source_port()
        self.baud = baud
        self.sample_rate = sample_rate
        self.duration_us = 7500 if abs(duration_ms - 7.5) < 0.5 else 10000
        self.num_channels = min(max(num_channels, 1), 6)
        self.octets = octets if octets > 0 else get_default_octets(self.sample_rate, self.duration_us)
        self.source_type = source_type
        self.mp3_dir = mp3_dir
        self.audio_device = audio_device
        self.sr_code = SAMPLE_RATE_CODES.get(self.sample_rate, 3)

        self.samples_per_frame = calculate_required_samples(self.sample_rate, self.duration_us)
        self.seq_counters = [0] * self.num_channels
        self.prev_lc3_frames = [bytes(self.octets)] * self.num_channels

        # Initialize array of LC3 encoders
        self.encoders = [
            LC3Encoder(frame_duration_us=self.duration_us, sample_rate_hz=self.sample_rate)
            for _ in range(self.num_channels)
        ]

        self.serial_conn = None
        self.audio_stream = None
        self.mp3_source = None
        self.audio_queue = collections.deque(maxlen=32)
        self.resampler = None
        self.is_running = False

    def open_serial(self):
        print(f"Connecting to ESP-NOW SOURCE on {self.port} @ {self.baud} baud...", flush=True)
        try:
            self.serial_conn = serial.Serial(
                self.port,
                self.baud,
                timeout=0.5,
                write_timeout=2.0,
                rtscts=False,
                dsrdtr=False
            )
            self.serial_conn.dtr = False
            self.serial_conn.rts = False
            time.sleep(0.3)
            self.serial_conn.reset_input_buffer()
            self.serial_conn.reset_output_buffer()
            print(f"Connected to {self.port}!", flush=True)
        except Exception as e:
            print(f"\n[ERROR] Failed to open serial port '{self.port}': {e}", flush=True)
            print("Tip: If using Node 21 DevKit, ensure you connect to the UART port (COM121) rather than COM21.\n", flush=True)
            raise

    def start_wasapi_capture(self):
        import sounddevice as sd
        devices = sd.query_devices()
        input_device_id = None
        
        if self.audio_device:
            for idx, dev in enumerate(devices):
                if self.audio_device.lower() in dev['name'].lower():
                    input_device_id = idx
                    break
        
        if input_device_id is None:
            default_out = sd.default.device[1]
            input_device_id = default_out if default_out is not None else 0

        dev_info = sd.query_devices(input_device_id)
        native_sr = int(dev_info.get('default_samplerate', 48000))
        native_ch = max(int(dev_info.get('max_output_channels', 2)), int(dev_info.get('max_input_channels', 2)))
        native_ch = max(native_ch, self.num_channels)

        print(f"Opening WASAPI Loopback on: '{dev_info['name']}' ({native_sr} Hz, {native_ch} ch)", flush=True)
        self.resampler = MultiChannelResampler(native_sr, self.sample_rate)

        def audio_callback(indata, frames, time_info, status):
            self.audio_queue.append(indata.copy())

        try:
            self.audio_stream = sd.InputStream(
                device=input_device_id,
                samplerate=native_sr,
                channels=min(native_ch, 6),
                dtype='float32',
                blocksize=int(native_sr * (self.duration_us / 1000000.0)),
                callback=audio_callback,
                extra_settings=sd.WasapiSettings(loopback=True) if hasattr(sd, 'WasapiSettings') else None
            )
            self.audio_stream.start()
        except Exception as e:
            print(f"Notice: Direct WASAPI loopback with extra_settings failed ({e}). Falling back to standard input stream...", flush=True)
            self.audio_stream = sd.InputStream(
                device=input_device_id,
                samplerate=native_sr,
                channels=min(native_ch, 6),
                dtype='float32',
                blocksize=int(native_sr * (self.duration_us / 1000000.0)),
                callback=audio_callback
            )
            self.audio_stream.start()

    def generate_synth_frames(self, phase_accs: list) -> np.ndarray:
        pcm = np.zeros((self.samples_per_frame, self.num_channels), dtype=np.int16)
        for ch in range(self.num_channels):
            freq = PENTATONIC_CH_FREQS[ch % len(PENTATONIC_CH_FREQS)]
            t = np.arange(self.samples_per_frame)
            phase = phase_accs[ch] + 2.0 * np.pi * freq * t / self.sample_rate
            phase_accs[ch] = (phase[-1] + 2.0 * np.pi * freq / self.sample_rate) % (2.0 * np.pi)
            
            signal_wave = np.sin(phase) * 0.25 * 32767.0
            pcm[:, ch] = signal_wave.astype(np.int16)
        return pcm

    def run(self, test_duration_sec: float = None):
        self.open_serial()
        self.is_running = True

        dur_str = f"{self.duration_us / 1000.0:.1f}ms"
        fps = 1000000.0 / self.duration_us
        bitrate_kbps = int((self.octets * 8 * 1000000) / (self.duration_us * 1000))

        print("\n" + "=" * 60)
        print("  Windows 11 Real-Time Audio Streamer for audioESP-NOW")
        print("=" * 60)
        print(f"  Target Port     : {self.port} @ {self.baud} baud")
        print(f"  Audio Source    : {self.source_type.upper()}")
        print(f"  Sample Rate     : {self.sample_rate} Hz (VSAF Code {self.sr_code})")
        print(f"  Frame Duration  : {dur_str} ({fps:.1f} fps)")
        print(f"  Channels        : {self.num_channels} (Ch 0 .. {self.num_channels - 1})")
        print(f"  LC3 Octets/Ch   : {self.octets} octets ({bitrate_kbps} kbps/ch)")
        print(f"  Total Bandwidth : {bitrate_kbps * self.num_channels} kbps")
        print("=" * 60 + "\n")

        if self.source_type == "mp3":
            try:
                self.mp3_source = Mp3FolderAudioSource(
                    folder_path=self.mp3_dir,
                    num_channels=self.num_channels,
                    sample_rate=self.sample_rate,
                    frame_duration_ms=self.duration_us / 1000.0
                )
            except Exception as e:
                print(f"[Notice] Failed to load MP3 folder '{self.mp3_dir}' ({e}). Falling back to 'synth' mode...", flush=True)
                self.source_type = "synth"

        if self.source_type == "wasapi" or self.source_type == "device":
            self.start_wasapi_capture()

        synth_phase = [0.0] * self.num_channels
        start_time = time.perf_counter()
        next_tick_ns = time.perf_counter_ns() + int(self.duration_us * 1000)
        step_ns = int(self.duration_us * 1000)

        total_frames = 0
        last_stat_time = time.perf_counter()
        frames_since_stat = 0

        dur_bit = 1 if self.duration_us == 7500 else 0

        if sys.platform == "win32":
            try:
                import ctypes
                kernel32 = ctypes.windll.kernel32
                kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
            except Exception:
                pass

        table_div  = "+----------+-------+----------+-----------+-------------+----------+"
        table_hdr1 = "| Time     | Total | Cadence  | Line Rate | Audio dBFS  | Channels |"
        table_hdr2 = "|          | Pkts  | (pkts/s) | (kbps)    | L | R (RMS) |          |"
        print(table_div, flush=True)
        print(table_hdr1, flush=True)
        print(table_hdr2, flush=True)
        print(table_div, flush=True)

        try:
            while self.is_running:
                now_ns = time.perf_counter_ns()
                if now_ns < next_tick_ns:
                    sleep_s = (next_tick_ns - now_ns) / 1e9
                    if sleep_s > 0.001:
                        time.sleep(sleep_s * 0.8)
                    while time.perf_counter_ns() < next_tick_ns:
                        pass
                next_tick_ns += step_ns

                # Acquire multi-channel PCM frame
                if self.source_type == "mp3" and self.mp3_source:
                    # mp3_source returns (channels, samples), transpose to (samples, channels)
                    raw_channels = self.mp3_source.get_frame()
                    pcm_frame = raw_channels.T
                elif (self.source_type == "wasapi" or self.source_type == "device") and len(self.audio_queue) > 0:
                    raw_data = self.audio_queue.popleft()
                    resampled = self.resampler.resample(raw_data)
                    pcm_frame = (np.clip(resampled, -1.0, 1.0) * 32767.0).astype(np.int16)
                    if len(pcm_frame) < self.samples_per_frame:
                        pad = np.zeros((self.samples_per_frame - len(pcm_frame), pcm_frame.shape[1]), dtype=np.int16)
                        pcm_frame = np.vstack([pcm_frame, pad])
                    elif len(pcm_frame) > self.samples_per_frame:
                        pcm_frame = pcm_frame[:self.samples_per_frame, :]
                else:
                    pcm_frame = self.generate_synth_frames(synth_phase)

                pts_us = int((time.perf_counter() * 1000000)) & 0xFFFFFFFF

                # Encode and transmit VSAF packet for each channel
                batch_bytes = bytearray()
                for ch in range(self.num_channels):
                    ch_pcm = pcm_frame[:, ch % pcm_frame.shape[1]]
                    curr_lc3 = self.encoders[ch].encode(ch_pcm, self.octets)
                    prev_lc3 = self.prev_lc3_frames[ch]

                    seq = self.seq_counters[ch] & 0xFF
                    self.seq_counters[ch] = (self.seq_counters[ch] + 1) & 0xFF
                    cfg = (ch & 0x07) | (self.sr_code << 3) | (dur_bit << 6) | (0 << 7)

                    # Assemble intact 248-byte VSAF packet (8B header + 120B curr + 120B prev)
                    header = struct.pack("<HBBI", VSAF_MAGIC, seq, cfg, pts_us)
                    curr_padded = curr_lc3.ljust(120, b'\x00')
                    prev_padded = prev_lc3.ljust(120, b'\x00')
                    pkt = header + curr_padded + prev_padded
                    batch_bytes.extend(pkt)

                    self.prev_lc3_frames[ch] = curr_lc3

                self.serial_conn.write(batch_bytes)
                total_frames += 1
                frames_since_stat += 1

                now = time.perf_counter()
                if now - last_stat_time >= 1.0:
                    dt = now - last_stat_time
                    fps_real = frames_since_stat / dt
                    pkts_sec = fps_real * self.num_channels
                    kbps_real = (frames_since_stat * len(batch_bytes) * 8) / (dt * 1000)
                    
                    ch0_data = pcm_frame[:, 0].astype(np.float32) / 32768.0
                    rms0 = np.sqrt(np.mean(ch0_data ** 2))
                    rms0_db = 20 * math.log10(rms0) if rms0 > 1e-5 else -99.9

                    if pcm_frame.shape[1] > 1:
                        ch1_data = pcm_frame[:, 1 % pcm_frame.shape[1]].astype(np.float32) / 32768.0
                        rms1 = np.sqrt(np.mean(ch1_data ** 2))
                        rms1_db = 20 * math.log10(rms1) if rms1 > 1e-5 else -99.9
                        rms_str = f"{rms0_db:>5.1f} {rms1_db:>5.1f}"
                    else:
                        rms_str = f"    {rms0_db:>5.1f}    "

                    time_str = time.strftime('%H:%M:%S')
                    total_pkts_num = total_frames * self.num_channels
                    pkts_str = f"{total_pkts_num}" if total_pkts_num < 100000 else f"{total_pkts_num/1000:.1f}k"
                    ch_str = f"{self.num_channels} ch"

                    line = f"| {time_str} | {pkts_str:>5} | {pkts_sec:>8.1f} | {kbps_real:>9.1f} | {rms_str} | {ch_str:>8} |"
                    sys.stdout.write(f"\r{line}")
                    sys.stdout.flush()

                    last_stat_time = now
                    frames_since_stat = 0

                if test_duration_sec and (now - start_time) >= test_duration_sec:
                    sys.stdout.write("\n" + table_div + "\n")
                    print(f"Completed test duration ({test_duration_sec}s). Stopping streamer...", flush=True)
                    break

        except KeyboardInterrupt:
            sys.stdout.write("\n" + table_div + "\n")
            print("Streamer interrupted by user.", flush=True)
        finally:
            self.close()

    def close(self):
        self.is_running = False
        if self.mp3_source:
            self.mp3_source.close()
        if self.audio_stream:
            try:
                self.audio_stream.stop()
                self.audio_stream.close()
            except Exception:
                pass
        if self.serial_conn and self.serial_conn.is_open:
            try:
                self.serial_conn.close()
            except Exception:
                pass
        print("Audio streamer stopped cleanly.", flush=True)


def main():
    default_mp3_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "data", "mp3"))

    parser = argparse.ArgumentParser(description="Real-Time PC Audio Streamer for audioESP-NOW (VSAF Protocol)")
    parser.add_argument("--port", type=str, default="COM121", help="Serial port of Node 21 SOURCE (default: COM121, or 'auto')")
    parser.add_argument("--baud", type=int, default=921600, help="Serial baud rate (default: 921600)")
    parser.add_argument("--sample-rate", type=int, default=48000, choices=[8000, 16000, 24000, 32000, 44100, 48000],
                        help="Target VSAF sample rate in Hz (default: 48000)")
    parser.add_argument("--duration", type=float, default=7.5, choices=[7.5, 10.0],
                        help="LC3 frame duration in ms (default: 7.5)")
    parser.add_argument("--channels", type=int, default=2, choices=[1, 2, 3, 4, 5, 6],
                        help="Number of audio channels to stream (1 to 6, default: 2)")
    parser.add_argument("--octets", type=int, default=0, help="LC3 octets per frame (0 = automatic default)")
    parser.add_argument("--source", type=str, default="mp3", choices=["mp3", "synth", "wasapi", "device"],
                        help="Audio input source: 'mp3' (Random tracks from data/mp3, default), 'synth', 'wasapi', or 'device'")
    parser.add_argument("--mp3-dir", type=str, default=default_mp3_dir,
                        help=f"Folder containing MP3 tracks (default: {default_mp3_dir})")
    parser.add_argument("--device", type=str, default=None, help="Name or partial string of Windows audio capture device")
    parser.add_argument("--test-duration", type=float, default=None, help="Optional duration in seconds to stream before exiting")

    args = parser.parse_args()

    streamer = PcAudioStreamer(
        port=args.port,
        baud=args.baud,
        sample_rate=args.sample_rate,
        duration_ms=args.duration,
        num_channels=args.channels,
        octets=args.octets,
        source_type=args.source,
        mp3_dir=args.mp3_dir,
        audio_device=args.device
    )
    streamer.run(test_duration_sec=args.test_duration)


if __name__ == "__main__":
    main()
