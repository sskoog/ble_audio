#!/usr/bin/env python3
"""
run_wroom32_benchmark.py
Connects to ESP-WROOM-32 on COM19 / CP210x device interface,
listens to or triggers the LC3 hardware benchmark suite (Fixed-Point & Float FPU),
and captures the results.
"""

import sys
import time
import serial
import serial.tools.list_ports

PORT_CANDIDATES = [
    r"\\?\USB#VID_10C4&PID_EA60#02SVFKOK#{86e0d1e0-8089-11d0-9ce4-08003e301f73}",
    "COM19",
]

BAUD = 115200
OUTPUT_FILE = "docs/wroom32_benchmark_raw.txt"

def open_serial():
    for p in PORT_CANDIDATES:
        try:
            print(f"Attempting to open port: {p}")
            s = serial.Serial(p, BAUD, timeout=1.0)
            print(f"Successfully opened {p}!")
            return s
        except Exception as e:
            print(f"Failed to open {p}: {e}")
    return None

def safe_print(s):
    try:
        sys.stdout.buffer.write((s + "\n").encode("utf-8", errors="replace"))
        sys.stdout.flush()
    except Exception:
        pass

def main():
    s = open_serial()
    if not s:
        print("Error: Could not open ESP32 serial port!")
        return 1

    print("Resetting ESP32 via RTS/DTR toggle...")
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    time.sleep(0.2)

    print("Listening for benchmark output...")
    lines = []
    start_time = time.time()
    in_results_table = False

    while True:
        raw = s.readline()
        if not raw:
            if time.time() - start_time > 350:
                print("\n[TIMEOUT] Benchmark exceeded 350 seconds.")
                break
            continue

        line = raw.decode("utf-8", errors="replace").strip()
        if line:
            safe_print(line)
            lines.append(line)

            if "ESP32-WROOM-32 HARDWARE LC3 ENCODER BENCHMARK RESULTS" in line:
                in_results_table = True

            if in_results_table and "Benchmark completed." in line:
                print("\n[SUCCESS] Benchmark suite completed!")
                break

        if time.time() - start_time > 350:
            print("\n[TIMEOUT] Benchmark exceeded 350 seconds.")
            break

    s.close()

    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"Saved raw log to {OUTPUT_FILE}")
    return 0

if __name__ == "__main__":
    exit(main())
