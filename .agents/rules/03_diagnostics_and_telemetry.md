# Rule: Diagnostics & Telemetry Conventions

1. **1 Hz Heartbeat Telemetry Format**:
   - SINK Heartbeat:
     `[NODE_ID] State: STREAMING | RSSI: -XX dBm | Pkts/s: XXX.X | Lost: X (PLC tot: X) | DMA UDR: X | FIFO UDR: X | PREV REC: X | Codec: X.XX ms | CPU: XX.X%`
   - SOURCE Heartbeat:
     `[SOURCE] Mode: STEREO | SR: 48.0k | PD: 10.0ms | Sent: XXX.X pkts/s (X.X kB/s) | Codec: X.XX ms | CPU: XX.X%`

2. **Counter Persistence**:
   - `PLC tot`, `DMA UDR`, `FIFO UDR`, and `PREV REC` must be cumulative session counters that only reset upon leaving the `STREAMING` state.
   - Instantaneous rates (such as `Lost 1/s`) must not overwrite cumulative diagnostic counters.

3. **Serial Command CLI Conventions**:
   - All interactive commands over USB serial must handle trailing `\r\n` cleanly and support:
     - `start` / `stop`: Toggle broadcasting state.
     - `sr <hz>`: Switch sample rate (48000, 32000, 24000, 16000, 8000).
     - `dur <ms>` / `pd <ms>`: Switch frame duration (10.0 or 7.5).
     - `mode <mono|stereo>`: Toggle channel mode.
     - `tone <hz>`: Adjust internal sine wave test frequency.
     - `vol <0-100>`: Adjust SINK software/hardware attenuation.
     - `stats` / `help`: Display diagnostic overview and command help.
