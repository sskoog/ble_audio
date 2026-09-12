#!/usr/bin/env python3
"""
========================================================================================
PC Real-Time LC3 Audio Streamer for audioESP-NOW Unicast Multi-Speaker Network
========================================================================================
Encodes and streams LC3 audio packets directly from the host PC over USB-Serial JTAG to
Node 16 (ESP32-S3 SOURCE in 'PC STRM' mode), which forwards:
  - Channel 0 (Left @ 48 kHz LC3, 120B) -> Node 23 (ESP32-C6 Left SINK)
  - Channel 5 (Subwoofer @ 8 kHz LC3, 80B via 200 Hz 4th-order LR-LP) -> Node 24 (ESP32-C6 Sub SINK)

Audio Source Modes:
  1. 'wasapi' / 'cable' / 'device': Live capture of Windows PC audio (Spotify, YouTube, Games, VLC).
  2. 'mp3': MP3 playlist player from data/mp3 folder.
  3. 'bass-test': Subwoofer crossover validation signal (50 Hz deep bass + 1 kHz melody).
  4. 'synth' / 'lfo': 0.2 Hz LFO dual-channel sine sweep (220-880 Hz Left, 440-1760 Hz Right).
"""

import sys
import os
import time
import math
import struct
import argparse
import queue
import threading
import random
import subprocess
import numpy as np
import scipy.signal

# Add bumble app path for liblc3 wrapper
BUMBLE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "usb_ble_bumble"))
if BUMBLE_DIR not in sys.path:
    sys.path.insert(0, BUMBLE_DIR)

try:
    import lc3_encoder
except ImportError:
    # Try local directory or relative paths
    alt_dirs = [
        r"C:\Git_ble_audio\apps\usb_ble_bumble",
        os.path.join(os.path.dirname(__file__), "..", "..", "apps", "usb_ble_bumble")
    ]
    imported = False
    for ad in alt_dirs:
        if os.path.exists(ad) and ad not in sys.path:
            sys.path.insert(0, ad)
            try:
                import lc3_encoder
                imported = True
                break
            except ImportError:
                pass
    if not imported:
        print("[ERROR] Could not import lc3_encoder. Ensure liblc3.dll is present in apps/usb_ble_bumble.")
        raise

try:
    import sounddevice as sd
except ImportError:
    print("[ERROR] sounddevice module is required. Run: pip install sounddevice")
    raise

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("[ERROR] pyserial module is required. Run: pip install pyserial")
    raise

VSAF_MAGIC = 0x1337
SAMPLE_RATE_48K = 48000
SAMPLE_RATE_8K = 8000
FRAME_DURATION_US = 10000 # 10.0 ms
SAMPLES_48K = 480
SAMPLES_8K = 80
OCTETS_LEFT_48K = 120
OCTETS_SUB_8K = 80
SUB_LP_CUTOFF_HZ = 200.0


def auto_detect_source_port() -> str:
    """Finds active ESP32-S3 (Node 16) COM port."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if "com16" in p.device.lower():
            return p.device
        if "usb-serial" in desc or "jtag" in desc or "303a" in hwid:
            return p.device
    return "COM16"


def find_audio_device(name_query: str = "cable output", prefer_input: bool = True):
    """Discovers audio device index matching name query."""
    devices = sd.query_devices()
    query = name_query.lower()
    
    # 1. Look for matching input device
    for idx, d in enumerate(devices):
        dname = d['name'].lower()
        if query in dname:
            if prefer_input and d['max_input_channels'] > 0:
                return idx, d
            elif not prefer_input and d['max_output_channels'] > 0:
                return idx, d

    # 2. Fallback to default
    default_id = sd.default.device[0 if prefer_input else 1]
    if default_id is not None and default_id >= 0:
        return default_id, devices[default_id]

    return 0, devices[0]


class Resampler:
    """Rational Polyphase Resampler (e.g. 44.1 kHz -> 48.0 kHz)."""
    def __init__(self, in_rate: int, out_rate: int):
        self.in_rate = in_rate
        self.out_rate = out_rate
        gcd = math.gcd(in_rate, out_rate)
        self.up = out_rate // gcd
        self.down = in_rate // gcd

    def resample(self, data: np.ndarray, target_samples: int) -> np.ndarray:
        if self.in_rate == self.out_rate and len(data) == target_samples:
            return data
        if len(data) == 0:
            return np.zeros((target_samples, data.shape[1] if data.ndim > 1 else 1), dtype=data.dtype)
        
        res = scipy.signal.resample_poly(data, self.up, self.down, axis=0)
        if len(res) < target_samples:
            pad_shape = list(res.shape)
            pad_shape[0] = target_samples - len(res)
            res = np.vstack([res, np.zeros(pad_shape, dtype=res.dtype)])
        elif len(res) > target_samples:
            res = res[:target_samples]
        return res


class PcUnicastStreamer:
    def __init__(
        self,
        port: str = "COM16",
        baud: int = 2000000,
        source_type: str = "synth",
        audio_device_name: str = "CABLE Output",
        mp3_folder: str = "data/mp3"
    ):
        self.port = auto_detect_source_port() if port.lower() in ("auto", "") else port
        self.baud = baud
        self.source_type = source_type.lower()
        self.audio_device_name = audio_device_name
        self.mp3_folder = mp3_folder

        self.serial_conn = None
        self.is_running = False
        self.audio_stream = None
        self.audio_queue = queue.Queue(maxsize=100)
        self.resampler = None

        # Google liblc3 Encoders:
        # Left Encoder: 48 kHz, 10.0 ms frame duration
        self.enc_left = lc3_encoder.LC3Encoder(FRAME_DURATION_US, SAMPLE_RATE_48K)
        # Subwoofer Encoder: 8 kHz, 10.0 ms frame duration
        self.enc_sub = lc3_encoder.LC3Encoder(FRAME_DURATION_US, SAMPLE_RATE_8K)

        # 4th-Order Linkwitz-Riley Lowpass Filter @ 200 Hz for Subwoofer
        nyq = 0.5 * SAMPLE_RATE_48K
        b_sub, a_sub = scipy.signal.butter(2, SUB_LP_CUTOFF_HZ / nyq, btype='low')
        self.sub_b = b_sub
        self.sub_a = a_sub
        self.sub_zi1 = scipy.signal.lfilter_zi(b_sub, a_sub)
        self.sub_zi2 = scipy.signal.lfilter_zi(b_sub, a_sub)

        # MP3 Player state
        self.mp3_proc = None
        self.mp3_playlist = []

        # Synth state
        self.lfo_phase = 0.0
        self.carrier_phase_l = 0.0
        self.carrier_phase_r = 0.0
        self.bass_phase = 0.0
        self.lead_phase = 0.0

    def open_serial(self):
        print(f"Connecting to SOURCE Dongle on {self.port} at {self.baud} baud...", flush=True)
        try:
            self.serial_conn = serial.Serial(
                self.port,
                self.baud,
                timeout=0.1,
                write_timeout=1.0,
                rtscts=False,
                dsrdtr=False
            )
            self.serial_conn.dtr = False
            self.serial_conn.rts = False
            time.sleep(0.1)
            self.serial_conn.reset_input_buffer()
            self.serial_conn.reset_output_buffer()
            print(f"SUCCESS: Connected to {self.port}!", flush=True)
        except Exception as e:
            print(f"[ERROR] Failed to open serial port '{self.port}': {e}", flush=True)
            raise

    def start_audio_source(self):
        if self.source_type in ("wasapi", "cable", "device"):
            dev_id, dev_info = find_audio_device(self.audio_device_name, prefer_input=True)
            native_sr = int(dev_info.get('default_samplerate', 48000))
            native_ch = max(int(dev_info.get('max_input_channels', 2)), 2)
            block_size = int(native_sr * (FRAME_DURATION_US / 1000000.0))

            print(f"Opening Audio Input on: [{dev_id}] '{dev_info['name']}' ({native_sr} Hz, {native_ch} ch)...", flush=True)
            if native_sr != SAMPLE_RATE_48K:
                self.resampler = Resampler(native_sr, SAMPLE_RATE_48K)
            else:
                self.resampler = None

            def callback(indata, frames, time_info, status):
                try:
                    self.audio_queue.put_nowait(indata.copy())
                except queue.Full:
                    pass

            if dev_info['max_input_channels'] > 0:
                self.audio_stream = sd.InputStream(
                    device=dev_id,
                    channels=min(native_ch, 2),
                    samplerate=native_sr,
                    dtype='float32',
                    blocksize=block_size,
                    callback=callback
                )
            else:
                self.audio_stream = sd.InputStream(
                    device=dev_id,
                    channels=2,
                    samplerate=native_sr,
                    dtype='float32',
                    blocksize=block_size,
                    callback=callback,
                    extra_settings=sd.WasapiSettings(loopback=True)
                )
            self.audio_stream.start()
            print("Live audio capture active.", flush=True)

        elif self.source_type == "mp3":
            self._init_mp3_player()

    def _init_mp3_player(self):
        valid_exts = (".mp3", ".wav", ".flac", ".m4a", ".aac", ".ogg")
        if os.path.exists(self.mp3_folder):
            self.mp3_playlist = [
                os.path.join(self.mp3_folder, f)
                for f in os.listdir(self.mp3_folder)
                if f.lower().endswith(valid_exts) and os.path.isfile(os.path.join(self.mp3_folder, f))
            ]
        if not self.mp3_playlist:
            print(f"[WARN] No MP3 files found in '{self.mp3_folder}'. Falling back to synth mode.")
            self.source_type = "synth"
            return
        self._play_next_mp3_track()

    def _play_next_mp3_track(self):
        if self.mp3_proc:
            try:
                self.mp3_proc.kill()
                self.mp3_proc.wait(timeout=0.1)
            except Exception:
                pass
            self.mp3_proc = None

        track = random.choice(self.mp3_playlist)
        clean_name = os.path.basename(track).encode('ascii', errors='replace').decode('ascii')
        print(f"\n[MP3 Track] Now Playing: '{clean_name}' (48 kHz Stereo)", flush=True)

        frame_bytes = SAMPLES_48K * 2 * 2
        cmd = [
            "ffmpeg",
            "-i", track,
            "-f", "s16le",
            "-acodec", "pcm_s16le",
            "-ar", "48000",
            "-ac", "2",
            "-loglevel", "quiet",
            "-"
        ]
        self.mp3_proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=frame_bytes * 10
        )

    def generate_synth_frame(self) -> np.ndarray:
        """Generates continuous-phase LFO sine sweep (220-880 Hz Left, 440-1760 Hz Right)."""
        t = np.arange(SAMPLES_48K) / SAMPLE_RATE_48K
        dt = SAMPLES_48K / SAMPLE_RATE_48K

        lfo_mod = np.sin(self.lfo_phase + 2.0 * np.pi * 0.2 * t)
        freq_l = 550.0 + 330.0 * lfo_mod
        freq_r = 1100.0 + 660.0 * lfo_mod

        phase_l = self.carrier_phase_l + 2.0 * np.pi * np.cumsum(freq_l) / SAMPLE_RATE_48K
        phase_r = self.carrier_phase_r + 2.0 * np.pi * np.cumsum(freq_r) / SAMPLE_RATE_48K
        self.carrier_phase_l = phase_l[-1] % (2.0 * np.pi)
        self.carrier_phase_r = phase_r[-1] % (2.0 * np.pi)
        self.lfo_phase = (self.lfo_phase + 2.0 * np.pi * 0.2 * dt) % (2.0 * np.pi)

        pcm_l = (np.sin(phase_l) * 0.6 * 32767.0).astype(np.int16)
        pcm_r = (np.sin(phase_r) * 0.6 * 32767.0).astype(np.int16)
        return np.column_stack([pcm_l, pcm_r])

    def generate_bass_test_frame(self) -> np.ndarray:
        """Generates 50 Hz deep bass on both channels + 1 kHz melody on Left."""
        t = np.arange(SAMPLES_48K) / SAMPLE_RATE_48K
        dt = SAMPLES_48K / SAMPLE_RATE_48K

        phase_bass = self.bass_phase + 2.0 * np.pi * 50.0 * t
        self.bass_phase = (self.bass_phase + 2.0 * np.pi * 50.0 * dt) % (2.0 * np.pi)
        bass_pcm = np.sin(phase_bass) * 0.7 * 32767.0

        phase_lead = self.lead_phase + 2.0 * np.pi * 1000.0 * t
        self.lead_phase = (self.lead_phase + 2.0 * np.pi * 1000.0 * dt) % (2.0 * np.pi)
        lead_pcm = np.sin(phase_lead) * 0.4 * 32767.0

        pcm_l = np.clip(bass_pcm + lead_pcm, -32768, 32767).astype(np.int16)
        pcm_r = np.clip(bass_pcm, -32768, 32767).astype(np.int16)
        return np.column_stack([pcm_l, pcm_r])

    def get_mp3_frame(self) -> np.ndarray:
        frame_bytes = SAMPLES_48K * 2 * 2
        if not self.mp3_proc:
            return np.zeros((SAMPLES_48K, 2), dtype=np.int16)
        raw_bytes = self.mp3_proc.stdout.read(frame_bytes)
        if len(raw_bytes) < frame_bytes:
            self._play_next_mp3_track()
            raw_bytes = self.mp3_proc.stdout.read(frame_bytes) if self.mp3_proc else b""
            if len(raw_bytes) < frame_bytes:
                return np.zeros((SAMPLES_48K, 2), dtype=np.int16)
        return np.frombuffer(raw_bytes, dtype=np.int16).reshape((SAMPLES_48K, 2))

    def run(self, test_duration_sec: float = None):
        self.open_serial()
        self.start_audio_source()
        self.is_running = True

        print("=" * 86)
        print("   PC REAL-TIME LC3 AUDIO STREAMER FOR audioESP-NOW UNICAST NETWORK")
        print("=" * 86)
        print(f"  Target Dongle   : {self.port} @ {self.baud} baud (Node 16 ESP32-S3 SOURCE)")
        print(f"  Audio Source    : {self.source_type.upper()} ({self.audio_device_name if self.source_type in ('wasapi','cable','device') else ''})")
        print(f"  Left SINK       : Node 23 (ESP32-C6) -> Channel 0 @ 48 kHz LC3 ({OCTETS_LEFT_48K}B = 96 kbps)")
        print(f"  Subwoofer SINK  : Node 24 (ESP32-C6) -> Channel 5 @ 8 kHz LC3 ({OCTETS_SUB_8K}B = 64 kbps, 200Hz LP)")
        print(f"  VSAF USB Framing: Ch 0 (130B) + Ch 5 (90B) = 220 Bytes/frame (100 fps = 176 kbps)")
        print("=" * 86)

        table_div  = "+----------+-------+----------+-----------+-------------------------------------+"
        table_hdr1 = "| Time     | Total | Cadence  | Line Rate |          Audio RMS (dBFS)           |"
        table_hdr2 = "|          | Pkts  | (pkts/s) | (kbps)    |   Left Ch      Right Ch     Subwoofer |"
        print(table_div, flush=True)
        print(table_hdr1, flush=True)
        print(table_hdr2, flush=True)
        print(table_div, flush=True)

        start_time = time.perf_counter()
        next_tick_ns = time.perf_counter_ns()
        step_ns = int(FRAME_DURATION_US * 1000)

        total_frames = 0
        frames_since_stat = 0
        last_stat_time = time.perf_counter()
        seq = 0

        # Prime initial buffer cushion for WASAPI capture
        if self.audio_stream is not None:
            time.sleep(0.04)

        try:
            while self.is_running:
                if test_duration_sec is not None and (time.perf_counter() - start_time) >= test_duration_sec:
                    break

                # 1. Acquire 10 ms Stereo PCM Frame
                if self.audio_stream is not None:
                    try:
                        raw_data = self.audio_queue.get(timeout=0.03)
                        if self.resampler:
                            resampled = self.resampler.resample(raw_data, SAMPLES_48K)
                        else:
                            resampled = raw_data
                        pcm_frame = (np.clip(resampled, -1.0, 1.0) * 32767.0).astype(np.int16)
                        if pcm_frame.ndim == 1:
                            pcm_frame = np.column_stack([pcm_frame, pcm_frame])
                        if pcm_frame.shape[0] < SAMPLES_48K:
                            pad = np.zeros((SAMPLES_48K - pcm_frame.shape[0], 2), dtype=np.int16)
                            pcm_frame = np.vstack([pcm_frame, pad])
                        elif pcm_frame.shape[0] > SAMPLES_48K:
                            pcm_frame = pcm_frame[:SAMPLES_48K, :]
                    except queue.Empty:
                        continue
                elif self.source_type == "mp3":
                    now_ns = time.perf_counter_ns()
                    if now_ns < next_tick_ns:
                        while time.perf_counter_ns() < next_tick_ns:
                            pass
                    next_tick_ns += step_ns
                    pcm_frame = self.get_mp3_frame()
                elif self.source_type == "bass-test":
                    now_ns = time.perf_counter_ns()
                    if now_ns < next_tick_ns:
                        while time.perf_counter_ns() < next_tick_ns:
                            pass
                    next_tick_ns += step_ns
                    pcm_frame = self.generate_bass_test_frame()
                else: # synth / lfo
                    now_ns = time.perf_counter_ns()
                    if now_ns < next_tick_ns:
                        while time.perf_counter_ns() < next_tick_ns:
                            pass
                    next_tick_ns += step_ns
                    pcm_frame = self.generate_synth_frame()

                pts_us = int((time.perf_counter() + 0.05) * 1000000) & 0xFFFFFFFF
                current_seq = seq & 0xFF
                seq = (seq + 1) & 0xFF

                # 2. Encode Left Channel (48 kHz LC3 -> 120 octets)
                pcm_left = pcm_frame[:, 0]
                lc3_left = self.enc_left.encode(pcm_left, OCTETS_LEFT_48K)

                # 3. Subwoofer DSP & 8 kHz LC3 Encoding (80 octets)
                sub_mono_f = (pcm_frame[:, 0].astype(np.float32) + pcm_frame[:, 1].astype(np.float32)) * 0.5
                sub_filt1, self.sub_zi1 = scipy.signal.lfilter(self.sub_b, self.sub_a, sub_mono_f, zi=self.sub_zi1)
                sub_filt2, self.sub_zi2 = scipy.signal.lfilter(self.sub_b, self.sub_a, sub_filt1, zi=self.sub_zi2)
                # Decimate 48 kHz (480 samples) -> 8 kHz (80 samples)
                sub_8k_pcm = scipy.signal.resample_poly(sub_filt2, 1, 6).astype(np.int16)
                if len(sub_8k_pcm) < SAMPLES_8K:
                    sub_8k_pcm = np.pad(sub_8k_pcm, (0, SAMPLES_8K - len(sub_8k_pcm)))
                elif len(sub_8k_pcm) > SAMPLES_8K:
                    sub_8k_pcm = sub_8k_pcm[:SAMPLES_8K]
                lc3_sub = self.enc_sub.encode(sub_8k_pcm, OCTETS_SUB_8K)

                # 4. Assemble VSAF LC3 Packets:
                # Header format (10B): Magic(uint16), Seq(uint8), Channel(uint8), Octets(uint8), Flags(uint8), PTS(uint32)
                # flags: Bit 0..2: SR code (4 for 48k, 0 for 8k), Bit 3: Dur (0 for 10ms)
                hdr_left = struct.pack("<HBBBB I", VSAF_MAGIC, current_seq, 0, OCTETS_LEFT_48K, 4, pts_us)
                pkt_left = hdr_left + lc3_left

                hdr_sub  = struct.pack("<HBBBB I", VSAF_MAGIC, current_seq, 5, OCTETS_SUB_8K, 0, pts_us)
                pkt_sub  = hdr_sub + lc3_sub

                # 5. Transmit both packets in one atomic USB batch (220 bytes total)
                batch = pkt_left + pkt_sub
                self.serial_conn.write(batch)

                total_frames += 1
                frames_since_stat += 1

                # 6. Dashboard Telemetry (1 Hz)
                now = time.perf_counter()
                if now - last_stat_time >= 1.0:
                    dt = now - last_stat_time
                    fps_real = frames_since_stat / dt
                    kbps_real = (frames_since_stat * len(batch) * 8) / (dt * 1000)

                    # Compute RMS dBFS
                    ch0 = pcm_frame[:, 0].astype(np.float32) / 32768.0
                    rms0 = np.sqrt(np.mean(ch0 ** 2))
                    rms0_db = 20 * math.log10(rms0) if rms0 > 1e-5 else -99.9

                    ch1 = pcm_frame[:, 1].astype(np.float32) / 32768.0
                    rms1 = np.sqrt(np.mean(ch1 ** 2))
                    rms1_db = 20 * math.log10(rms1) if rms1 > 1e-5 else -99.9

                    sub_rms = np.sqrt(np.mean((sub_filt2 / 32768.0) ** 2))
                    rms_sub_db = 20 * math.log10(sub_rms) if sub_rms > 1e-5 else -99.9

                    elapsed = now - start_time
                    m, s = divmod(int(elapsed), 60)
                    time_str = f"{m:02d}:{s:02d}.{int((elapsed % 1) * 10)}"

                    def vu_meter(val_db, width=6):
                        if val_db < -60: return "." * width
                        frac = max(0.0, min(1.0, (val_db + 60.0) / 60.0))
                        bars = int(round(frac * width))
                        return "#" * bars + "." * (width - bars)

                    print(f"| {time_str:<8} | {total_frames:>5} | {fps_real:>6.1f}/s  | {kbps_real:>6.0f} kbps | "
                          f"[{vu_meter(rms0_db)}] {rms0_db:>5.1f}dB  "
                          f"[{vu_meter(rms1_db)}] {rms1_db:>5.1f}dB  "
                          f"[{vu_meter(rms_sub_db)}] {rms_sub_db:>5.1f}dB |", flush=True)

                    frames_since_stat = 0
                    last_stat_time = now

        except KeyboardInterrupt:
            print("\nStream stopped by user.")
        finally:
            self.close()

    def close(self):
        self.is_running = False
        if self.audio_stream is not None:
            try: self.audio_stream.stop(); self.audio_stream.close()
            except Exception: pass
        if self.mp3_proc is not None:
            try: self.mp3_proc.kill()
            except Exception: pass
        if self.serial_conn is not None and self.serial_conn.is_open:
            try: self.serial_conn.close()
            except Exception: pass
        print("[Streamer] Shutdown complete.")


def main():
    parser = argparse.ArgumentParser(description="PC Real-Time LC3 Audio Streamer for audioESP-NOW Unicast")
    parser.add_argument("--port", type=str, default="COM16", help="Target Dongle Port (default: COM16 or 'auto')")
    parser.add_argument("--baud", type=int, default=2000000, help="Serial Baud Rate (default: 2000000)")
    parser.add_argument("--source", type=str, default="wasapi", choices=["wasapi", "cable", "device", "mp3", "bass-test", "synth"],
                        help="Audio Source Mode (default: wasapi)")
    parser.add_argument("--device", type=str, default="CABLE Output", help="Audio device query name for WASAPI/Cable capture")
    parser.add_argument("--mp3-folder", type=str, default="data/mp3", help="Folder containing MP3 audio files (default: data/mp3)")
    parser.add_argument("--duration", type=float, default=None, help="Streaming duration in seconds (optional, runs indefinitely if omitted)")
    args = parser.parse_args()

    streamer = PcUnicastStreamer(
        port=args.port,
        baud=args.baud,
        source_type=args.source,
        audio_device_name=args.device,
        mp3_folder=args.mp3_folder
    )
    streamer.run(test_duration_sec=args.duration)


if __name__ == "__main__":
    main()
