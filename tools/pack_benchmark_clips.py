#!/usr/bin/env python3
"""
pack_benchmark_clips.py
Prepares 10.0s 16-bit mono PCM clips from 'data/10s clips/10s 48 kHz mono CLIP Monster.wav'
for sample rates: 48000, 44100, 32000, 24000, 16000, 8000 Hz.
Packages them into benchmark_clips.bin with an indexed header for ESP32-C6 flash partition.
"""

import os
import struct
import subprocess
import wave

SOURCE_WAV = r"data/10s clips/10s 48 kHz mono CLIP Monster.wav"
OUTPUT_BIN = r"data/benchmark_clips.bin"
SAMPLE_RATES = [48000, 44100, 32000, 24000, 16000, 8000]

MAGIC = 0x4C433342  # 'LC3B' in hex
VERSION = 1
HEADER_SIZE = 4096   # 4 KB aligned header

def main():
    if not os.path.exists(SOURCE_WAV):
        print(f"Error: Source file not found: {SOURCE_WAV}")
        return 1

    clip_data = []
    
    for sr in SAMPLE_RATES:
        temp_wav = f"data/temp_{sr}.wav"
        print(f"Generating {sr} Hz 16-bit mono 10s clip...")
        cmd = [
            "ffmpeg", "-y", "-i", SOURCE_WAV,
            "-ar", str(sr), "-ac", "1", "-c:a", "pcm_s16le",
            "-t", "10.0", temp_wav
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"FFmpeg error: {res.stderr}")
            return 1

        with wave.open(temp_wav, "rb") as w:
            n_frames = w.getnframes()
            raw_pcm = w.readframes(n_frames)
            # Ensure exact 10s: exactly sr * 10 samples
            target_samples = sr * 10
            target_bytes = target_samples * 2
            if len(raw_pcm) > target_bytes:
                raw_pcm = raw_pcm[:target_bytes]
            elif len(raw_pcm) < target_bytes:
                raw_pcm += b"\x00" * (target_bytes - len(raw_pcm))

            print(f"  -> {sr} Hz: {len(raw_pcm)} bytes ({len(raw_pcm)//2} samples = {len(raw_pcm)/2/sr:.3f}s)")
            clip_data.append((sr, raw_pcm))

        if os.path.exists(temp_wav):
            os.remove(temp_wav)

    # Header format:
    # uint32_t magic (LC3B)
    # uint32_t version
    # uint32_t clip_count
    # uint32_t header_size
    # 8 entries of:
    #   uint32_t sample_rate
    #   uint32_t offset
    #   uint32_t size_bytes
    #   uint32_t sample_count

    header = bytearray(HEADER_SIZE)
    struct.pack_into("<IIII", header, 0, MAGIC, VERSION, len(SAMPLE_RATES), HEADER_SIZE)

    current_offset = HEADER_SIZE
    for idx, (sr, pcm) in enumerate(clip_data):
        entry_offset = 16 + idx * 16
        size_bytes = len(pcm)
        sample_count = size_bytes // 2
        struct.pack_into("<IIII", header, entry_offset, sr, current_offset, size_bytes, sample_count)
        current_offset += size_bytes

    with open(OUTPUT_BIN, "wb") as f:
        f.write(header)
        for _, pcm in clip_data:
            f.write(pcm)

    total_size = os.path.getsize(OUTPUT_BIN)
    print(f"\nSuccessfully generated {OUTPUT_BIN}")
    print(f"Total binary size: {total_size} bytes ({total_size / (1024*1024):.2f} MB)")
    print(f"Packed {len(clip_data)} clips with header.")
    return 0

if __name__ == "__main__":
    exit(main())
