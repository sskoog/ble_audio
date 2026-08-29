#!/usr/bin/env python3
"""
run_s3_benchmark.py
Connects to Seeed Studio XIAO ESP32-S3 on COM3 / COM16,
monitors the automated benchmark execution and captures the full results.
"""

import sys
import time
import serial
import serial.tools.list_ports

PORT_CANDIDATES = ["COM3", "COM16"]
BAUD = 115200
OUTPUT_FILE = "docs/s3_benchmark_raw.txt"

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
        print("Error: Could not open ESP32-S3 serial port!")
        return 1

    time.sleep(0.5)
    print("Triggering / checking benchmark on ESP32-S3...")
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

            if "HARDWARE LC3 ENCODER BENCHMARK RESULTS" in line:
                in_results_table = True

            if in_results_table and "+======================================================================================================================================+" in line and len(lines) > 40:
                # Wait for any trailing prints
                time.sleep(2.0)
                print("\n[SUCCESS] Benchmark table captured from ESP32-S3!")
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
