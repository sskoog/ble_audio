"""
Audio Source Providers for BLE Audio Broadcaster
Supports:
1. LfoSineAudioSource: Continuous 0.2 Hz LFO Sine Sweep (220 Hz - 880 Hz) @ 25% amplitude.
2. VirtualCableAudioSource / DeviceAudioSource: Real-time PCM audio capture from Windows audio devices (Virtual Cable).
"""

import math
import time
import queue
import numpy as np
import sounddevice as sd
from typing import Optional, List, Tuple


class AudioSource:
    """Base interface for 10ms PCM audio frame generators."""
    def get_frame(self) -> np.ndarray:
        """
        Returns a NumPy array of shape (num_channels, num_samples) of type np.int16.
        For 48kHz 10ms framing, num_samples = 480.
        """
        raise NotImplementedError

    def close(self):
        pass


class LfoSineAudioSource(AudioSource):
    """
    Synthesizes a continuous sine wave with frequency modulated by an LFO.
    - Amplitude: 25% (Peak = 0.25 * 32767 ≈ 8192)
    - LFO Frequency: 0.2 Hz (5-second period)
    - Sweep Range: 220 Hz (A3) to 880 Hz (A5)
    - Center Frequency: 550 Hz, Modulation Depth: 330 Hz
    """
    def __init__(
        self,
        num_channels: int = 2,
        sample_rate: int = 48000,
        frame_duration_ms: float = 10.0,
        amplitude: float = 0.25,
        lfo_rate_hz: float = 0.2,
        freq_min_hz: float = 220.0,
        freq_max_hz: float = 880.0
    ):
        self.num_channels = num_channels
        self.sample_rate = sample_rate
        self.frame_samples = int(sample_rate * frame_duration_ms / 1000.0)
        self.amplitude = amplitude * 32767.0

        self.lfo_rate = lfo_rate_hz
        self.center_freq = (freq_min_hz + freq_max_hz) / 2.0
        self.mod_depth = (freq_max_hz - freq_min_hz) / 2.0

        self.phase = 0.0
        self.lfo_phase = 0.0
        self.dt = 1.0 / sample_rate

    def get_frame(self) -> np.ndarray:
        samples = np.empty((self.num_channels, self.frame_samples), dtype=np.int16)

        # Generate sample by sample with phase integration for seamless frequency modulation
        mono_buf = np.empty(self.frame_samples, dtype=np.float32)
        phase = self.phase
        lfo_phase = self.lfo_phase
        lfo_dphase = 2.0 * math.pi * self.lfo_rate * self.dt
        two_pi = 2.0 * math.pi

        for i in range(self.frame_samples):
            # Instantaneous frequency modulated by LFO
            inst_freq = self.center_freq + self.mod_depth * math.sin(lfo_phase)
            mono_buf[i] = math.sin(phase)

            # Advance phases
            phase += 2.0 * math.pi * inst_freq * self.dt
            lfo_phase += lfo_dphase

            if phase >= two_pi:
                phase -= two_pi
            if lfo_phase >= two_pi:
                lfo_phase -= two_pi

        self.phase = phase
        self.lfo_phase = lfo_phase

        mono_s16 = np.clip(mono_buf * self.amplitude, -32768, 32767).astype(np.int16)

        for ch in range(self.num_channels):
            samples[ch, :] = mono_s16

        return samples


class DeviceAudioSource(AudioSource):
    """
    Captures live PCM audio from a Windows sound device (e.g. VB-Audio Virtual Cable).
    """
    def __init__(
        self,
        device_query: Optional[str] = None,
        num_channels: int = 2,
        sample_rate: int = 48000,
        frame_duration_ms: float = 10.0
    ):
        self.num_channels = num_channels
        self.sample_rate = sample_rate
        self.frame_samples = int(sample_rate * frame_duration_ms / 1000.0)

        # Resolve device
        target_device = self._find_input_device(device_query)
        self.device_info = sd.query_devices(target_device)
        self.device_name = self.device_info['name']
        max_in_ch = self.device_info['max_input_channels']

        if max_in_ch < 1:
            raise ValueError(f"Device '{self.device_name}' (ID {target_device}) is an output-only device.")

        self.stream_channels = min(num_channels, max_in_ch)
        self.audio_queue = queue.Queue(maxsize=100) # ~1 second buffer

        # Start input stream
        self.stream = sd.InputStream(
            device=target_device,
            channels=self.stream_channels,
            samplerate=sample_rate,
            blocksize=self.frame_samples,
            dtype='int16',
            callback=self._audio_callback
        )
        self.stream.start()

    def _find_input_device(self, query: Optional[str]) -> int:
        devices = sd.query_devices()

        # Direct integer device ID
        if query is not None:
            try:
                dev_id = int(query)
                if 0 <= dev_id < len(devices) and devices[dev_id]['max_input_channels'] > 0:
                    return dev_id
            except ValueError:
                pass

        query_lower = (query or "cable").lower()

        # 1. Look for WASAPI matches first (hostapi 2 on Windows)
        for idx, dev in enumerate(devices):
            if dev['max_input_channels'] > 0 and dev.get('hostapi') == 2 and query_lower in dev['name'].lower():
                return idx

        # 2. Look for DirectSound / other matches
        for idx, dev in enumerate(devices):
            if dev['max_input_channels'] > 0 and query_lower in dev['name'].lower():
                return idx

        # 3. Fallback to default input
        default_in = sd.default.device[0]
        if default_in is not None and default_in >= 0:
            return default_in

        for idx, dev in enumerate(devices):
            if dev['max_input_channels'] > 0:
                return idx

        raise ValueError(f"Audio input device matching '{query}' not found.")

    def _audio_callback(self, indata, frames, time_info, status):
        try:
            # indata shape: (frames, channels)
            self.audio_queue.put_nowait(indata.copy())
        except queue.Full:
            pass

    def get_frame(self) -> np.ndarray:
        try:
            data = self.audio_queue.get_nowait()
            # data is (frame_samples, stream_channels)
            # transpose to (stream_channels, frame_samples)
            out = np.zeros((self.num_channels, self.frame_samples), dtype=np.int16)
            for ch in range(self.stream_channels):
                out[ch, :] = data[:, ch]
            # If requesting more channels than device provides, duplicate channel 0
            for ch in range(self.stream_channels, self.num_channels):
                out[ch, :] = out[0, :]
            return out
        except queue.Empty:
            # Underflow: return silence
            return np.zeros((self.num_channels, self.frame_samples), dtype=np.int16)

    def close(self):
        try:
            self.stream.stop()
            self.stream.close()
        except Exception:
            pass


def list_audio_devices():
    """Prints all available input and output devices."""
    devices = sd.query_devices()
    print("Available Audio Devices:")
    for idx, dev in enumerate(devices):
        in_ch = dev['max_input_channels']
        out_ch = dev['max_output_channels']
        kind = []
        if in_ch > 0: kind.append(f"{in_ch} in")
        if out_ch > 0: kind.append(f"{out_ch} out")
        print(f"  [{idx:2d}] {dev['name']} ({', '.join(kind)}) - Host API: {dev['hostapi']}")
