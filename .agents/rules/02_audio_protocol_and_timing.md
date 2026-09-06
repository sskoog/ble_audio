---
trigger: always_on
---

# Rule: Audio Protocol & Timing Standards

1. **Hardware Timer Pacing**:
   - Periodic audio generation tasks MUST calculate absolute next execution deadlines using `esp_timer_get_time()`.
   - Never use relative delays (`vTaskDelay`) or FreeRTOS tick-based pacing for LC3 packet generation, as FreeRTOS tick granularity (100 Hz / 10 ms or 1000 Hz / 1 ms) causes cumulative frame creep and sink FIFO underruns.

2. **Buffer and Packet Sizing**:
   - Max LC3 frame octets: 120 bytes. 
   - Max PCM samples per frame: 480 samples (10 ms @ 48 kHz).
   - Ring buffers for I2S DMA must maintain at least 4x frame duration headroom (e.g., 40 ms buffer) with prefill threshold set to 5 frames (50 ms).

3. **Packet Loss Concealment (PLC)**:
   - SINK nodes must track missing sequence numbers.
   - If frame N is missing but frame N-1 payload is present in packet N+1, decode the redundant frame.
   - If both are missing, invoke `lc3_decode(..., NULL, pcm_out)` for standard PLC interpolation.
