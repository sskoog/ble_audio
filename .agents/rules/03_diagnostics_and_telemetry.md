---
trigger: always_on
---

# Rule: Diagnostics & Telemetry Conventions

1. **Heartbeat Telemetry**: For embedded nodes, generally include a heartbeat printout via USB serial port with 1.0 s period time. Format all parameters as constant-width for easy readout over multiple lines. Example parameters to print:
   - Node uptime (ms), CPU temperature, CPU load %, system state, WiFi/Bluetooth status, SSRI, status of buffers/overruns/underruns.

