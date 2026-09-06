---
trigger: always_on
---

# Rule: Build and Flash Operations (ESP-IDF v6.0.2)

1. **Environment Activation**:
   - The workspace standard is **ESP-IDF v6.0.2** installed at `C:\Users\stefa\OneDrive\Documents\ESP\v6.0.2\esp-idf` with tools in `C:\Users\stefa\OneDrive\Documents\ESP\.esptools` or `C:\Users\stefa\.espressif`.
   - Always activate the environment using:
     ```powershell
     if (Test-Path "C:\Users\stefa\OneDrive\Documents\ESP\.esptools") {
         $env:IDF_TOOLS_PATH="C:\Users\stefa\OneDrive\Documents\ESP\.esptools"
     } else {
         $env:IDF_TOOLS_PATH="C:\Users\stefa\.espressif"
     }

     if (Test-Path "$env:IDF_TOOLS_PATH\python_env\idf6.0_py3.13_env") {
         $env:IDF_PYTHON_ENV_PATH="$env:IDF_TOOLS_PATH\python_env\idf6.0_py3.13_env"
     } else {
         $env:IDF_PYTHON_ENV_PATH="$env:IDF_TOOLS_PATH\python_env\idf6.0_py3.11_env"
     }
     $env:PATH="$env:IDF_PYTHON_ENV_PATH\Scripts;" + $env:PATH

     if (Test-Path "C:\Users\stefa\OneDrive\Documents\ESP\v6.0.2\esp-idf\export.ps1") {
         . "C:\Users\stefa\OneDrive\Documents\ESP\v6.0.2\esp-idf\export.ps1"
     } else {
         . "C:\Users\stefa\OneDrive\Documents\ESP\v6.0.2\export.ps1"
     }
     ```

2. **Target and Configuration Separation**:
   - ESP32-S3 uses `build_s3` and `sdkconfig.s3` (4 MB flash).
   - ESP32-C6 uses `build_c6` and `sdkconfig.c6` (8 MB flash).
   - Always copy the matching `sdkconfig.<target>` to `sdkconfig` before running `idf.py build`.

3. **COM-ports**
   - Embedded devices are normally connected to this local PC via COM-port, where the COM-port number is assigned according to the node ID, i.e. node12 = COM12.
   - COM-ports are used for both flashing and for debugging via virtual serial port. Be aware of resource conflicts when flashing, e.g. locked COM-port from a serial interface.

4. **Port Locking Mitigation**:
   - Before invoking `esptool.py` or flash scripts, terminate all lingering Python scripts and serial monitors holding COM port handles:
     `Get-CimInstance Win32_Process -Filter "CommandLine LIKE '%device monitor%' OR CommandLine LIKE '%idf_monitor%' OR CommandLine LIKE '%test_audio_matrix%' OR CommandLine LIKE '%pc_audio_streamer%'" | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }`

5. **Flashing Parameters**:
   - Always use baud rate `460800`, `--connect-attempts 10`, `--before default_reset`, and `--after hard_reset`.
   - After flashing, allow a 2.5 to 3.0 second delay for Windows USB CDC / Serial JTAG re-enumeration before attempting to open the serial port.