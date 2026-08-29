#!/usr/bin/env python3
"""
run_c6_benchmark.py
Connects to ESP32-C6 (Node 21) on COM121 or USB-Serial COM21,
sends 'bench' command, and captures the benchmark output.
"""

import sys
import time
import serial

PORT_CANDIDATES = ["COM121", "COM21"]
BAUD = 115200
OUTPUT_FILE = "docs/c6_benchmark_raw.txt"

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
        print("Error: Could not open ESP32-C6 serial port!")
        return 1

    time.sleep(0.5)
    print("Sending 'bench' command to ESP32-C6...")
    s.write(b"\r\nbench\r\n")
    s.flush()

    lines = []
    start_time = time.time()
    in_results_table = False

    while True:
        raw = s.readline()
        if not raw:
            if time.time() - start_time > 300:
                print("\n[TIMEOUT] Benchmark exceeded 300 seconds.")
                break
            continue

        line = raw.decode("utf-8", errors="replace").strip()
        if line:
            safe_print(line)
            lines.append(line)

            if "ESP32-C6 HARDWARE LC3 ENCODER BENCHMARK RESULTS" in line:
                in_results_table = True

            if in_results_table and "+======================================================================================================================+" in line and len(lines) > 25:
                # Wait for any trailing prints
                time.sleep(2.0)
                print("\n[SUCCESS] Benchmark table captured!")
                break

        if time.time() - start_time > 300:
            print("\n[TIMEOUT] Benchmark exceeded 300 seconds.")
            break

    s.close()

    with open(OUTPUT_FILE, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"Saved raw log to {OUTPUT_FILE}")
    return 0

if __name__ == "__main__":
    exit(main())
