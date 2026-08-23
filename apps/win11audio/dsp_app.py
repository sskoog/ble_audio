import argparse
import sys
import numpy as np
import scipy.signal as signal
import matplotlib.pyplot as plt
import sounddevice as sd
import dsp_engine

# Audio sample rate (Hz)
FS = 48000
# Block size (number of samples per processing block)
# More blocks = more efficient processing, but more latency and memory usage.
BLOCK_SIZE = 512

def calculate_tf_and_group_delay(input_sig, output_sig, fs=FS, nperseg=4096):
    """Calculates frequency transfer function magnitude (dB) and group delay (ms)."""
    freqs, h_out = signal.freqz(output_sig, a=1.0, worN=nperseg, fs=fs)
    _, h_in = signal.freqz(input_sig, a=1.0, worN=nperseg, fs=fs)
    
    tf = h_out / np.where(np.abs(h_in) < 1e-12, 1e-12, h_in)
    mag_db = 20.0 * np.log10(np.clip(np.abs(tf), 1e-6, 1e3))
    
    phase = np.unwrap(np.angle(tf))
    df = freqs[1] - freqs[0]
    dw = 2.0 * np.pi * df
    group_delay_ms = (-np.gradient(phase, dw) / fs) * 1000.0
    
    return freqs, mag_db, group_delay_ms

def run_analytical_mode(save_plot=None):
    pipeline = dsp_engine.AudioDSPPipeline(float(FS))
    pipeline.set_mode(dsp_engine.MixingMode.MODE_PHASE_SHIFT)
    pipeline.set_phase_shift(0.0) # 0 rad static reference for linear transfer verification

    n_samples = 8192
    stimulus_l = np.zeros(n_samples, dtype=np.float32)
    stimulus_r = np.zeros(n_samples, dtype=np.float32)
    stimulus_l[0] = 1.0
    stimulus_r[0] = 0.5

    results = pipeline.process_frames(stimulus_l, stimulus_r)

    # 1. Raw Signals
    f, mag_raw_l, gd_raw_l = calculate_tf_and_group_delay(stimulus_l, results["raw_l"])
    _, mag_raw_r, gd_raw_r = calculate_tf_and_group_delay(stimulus_r, results["raw_r"])

    # 2. Mid & Side (Pre-Hilbert)
    mid_ref = 0.5 * (stimulus_l + stimulus_r)
    side_ref = 0.5 * (stimulus_l - stimulus_r)
    _, mag_mid_pre, gd_mid_pre = calculate_tf_and_group_delay(mid_ref, results["mid_pre"])
    _, mag_side_pre, gd_side_pre = calculate_tf_and_group_delay(side_ref, results["side_pre"])

    # 3. Mid & Side Post-Hilbert (In-Phase Real Component)
    mid_i = np.real(results["mid_post"])
    side_i = np.real(results["side_post"])
    _, mag_mid_post, gd_mid_post = calculate_tf_and_group_delay(mid_ref, mid_i)
    _, mag_side_post, gd_side_post = calculate_tf_and_group_delay(side_ref, side_i)

    # 3-Row Subplot Visualization
    fig, axes = plt.subplots(3, 2, figsize=(14, 10), sharex=True)

    # Row 1: Raw
    axes[0, 0].semilogx(f, mag_raw_l, label='Raw Left', color='tab:blue')
    axes[0, 0].semilogx(f, mag_raw_r, label='Raw Right', color='tab:cyan', linestyle='--')
    axes[0, 0].set_ylabel('Magnitude (dB)')
    axes[0, 0].set_title('1. Raw L/R: Magnitude')
    axes[0, 0].grid(True, which='both', linestyle=':')
    axes[0, 0].legend()

    axes[0, 1].semilogx(f, gd_raw_l, label='Raw Left', color='tab:blue')
    axes[0, 1].semilogx(f, gd_raw_r, label='Raw Right', color='tab:cyan', linestyle='--')
    axes[0, 1].set_ylabel('Group Delay (ms)')
    axes[0, 1].set_title('1. Raw L/R: Group Delay')
    axes[0, 1].grid(True, which='both', linestyle=':')
    axes[0, 1].legend()

    # Row 2: Pre-Hilbert (100 Hz HPF)
    axes[1, 0].semilogx(f, mag_mid_pre, label='Mid', color='tab:green')
    axes[1, 0].semilogx(f, mag_side_pre, label='Side', color='tab:olive', linestyle='--')
    axes[1, 0].axvline(100, color='r', linestyle=':', label='100 Hz HPF')
    axes[1, 0].set_ylabel('Magnitude (dB)')
    axes[1, 0].set_title('2. Mid/Side Pre-Hilbert: 100 Hz HPF')
    axes[1, 0].grid(True, which='both', linestyle=':')
    axes[1, 0].legend()

    axes[1, 1].semilogx(f, gd_mid_pre, label='Mid', color='tab:green')
    axes[1, 1].semilogx(f, gd_side_pre, label='Side', color='tab:olive', linestyle='--')
    axes[1, 1].set_ylabel('Group Delay (ms)')
    axes[1, 1].set_title('2. Mid/Side Pre-Hilbert: HPF Phase Delay')
    axes[1, 1].grid(True, which='both', linestyle=':')
    axes[1, 1].legend()

    # Row 3: Post-Hilbert Analytic In-Phase ($I$)
    axes[2, 0].semilogx(f, mag_mid_post, label='Mid Analytic (I)', color='tab:red')
    axes[2, 0].semilogx(f, mag_side_post, label='Side Analytic (I)', color='tab:purple', linestyle='--')
    axes[2, 0].set_xlabel('Frequency (Hz)')
    axes[2, 0].set_ylabel('Magnitude (dB)')
    axes[2, 0].set_title('3. Mid/Side Post-Hilbert: Analytic Real Path')
    axes[2, 0].grid(True, which='both', linestyle=':')
    axes[2, 0].legend()

    axes[2, 1].semilogx(f, gd_mid_post, label='Mid Analytic (I)', color='tab:red')
    axes[2, 1].semilogx(f, gd_side_post, label='Side Analytic (I)', color='tab:purple', linestyle='--')
    axes[2, 1].axhline(15.0 / FS * 1000.0, color='gray', linestyle=':', label='Matched FIR Delay (0.31 ms)')
    axes[2, 1].set_xlabel('Frequency (Hz)')
    axes[2, 1].set_ylabel('Group Delay (ms)')
    axes[2, 1].set_title('3. Mid/Side Post-Hilbert: Aligned Group Delay')
    axes[2, 1].grid(True, which='both', linestyle=':')
    axes[2, 1].legend()

    plt.xlim(20.0, 20000.0)
    plt.tight_layout()
    if save_plot:
        plt.savefig(save_plot, dpi=150)
        print(f"Analytical plot saved to: {save_plot}")
    else:
        plt.show()

def list_audio_devices():
    """Prints all host audio devices with index and channel info."""
    print("=== Available Audio Devices ===")
    devices = sd.query_devices()
    for idx, d in enumerate(devices):
        host_api = sd.query_hostapis(d['hostapi'])['name']
        in_ch = d['max_input_channels']
        out_ch = d['max_output_channels']
        default_mark = ""
        if idx == sd.default.device[0]:
            default_mark += " [Default Input]"
        if idx == sd.default.device[1]:
            default_mark += " [Default Output]"
        print(f"[{idx:2d}] {d['name']} ({host_api}) - In: {in_ch}, Out: {out_ch}{default_mark}")
    print()

def resolve_device(target_device=None, loopback=False):
    """Resolves device by index or name substring, configuring WASAPI loopback if requested."""
    devices = sd.query_devices()
    dev_id = None
    extra_settings = None

    if target_device is not None:
        try:
            dev_id = int(target_device)
        except ValueError:
            matches = [i for i, d in enumerate(devices) if target_device.lower() in d['name'].lower()]
            if matches:
                dev_id = matches[0]

    if loopback:
        if dev_id is None:
            wasapi_api_idx = next((i for i, h in enumerate(sd.query_hostapis()) if "wasapi" in h['name'].lower()), None)
            if wasapi_api_idx is not None:
                dev_id = next((i for i, d in enumerate(devices) if d['hostapi'] == wasapi_api_idx and d['max_output_channels'] >= 2), sd.default.device[1])
            else:
                dev_id = sd.default.device[1]
        extra_settings = sd.WasapiSettings(loopback=True)
        print(f"[Audio Stream] Capturing via WASAPI Loopback on Device [{dev_id}]: {devices[dev_id]['name']}")
    else:
        if dev_id is None:
            dev_id = sd.default.device[0]
        print(f"[Audio Stream] Capturing from Input Device [{dev_id}]: {devices[dev_id]['name']}")

    return dev_id, extra_settings

def run_streaming_mode(target_device=None, mode=2, loopback=False,
                       phase_shift_deg=45.0, lfo_freq_hz=1.5, depth_deg=90.0, dfs_offset_hz=5.0):
    pipeline = dsp_engine.AudioDSPPipeline(float(FS))
    
    if mode == 0:
        rad = np.radians(phase_shift_deg)
        pipeline.set_mode(dsp_engine.MixingMode.MODE_PHASE_SHIFT)
        pipeline.set_phase_shift(rad)
        print(f"[DSP Mode] 0: Static Phase Shift ({phase_shift_deg:.1f} deg)")
    elif mode == 1:
        depth_rad = np.radians(depth_deg)
        pipeline.set_mode(dsp_engine.MixingMode.MODE_PHASE_MODULATOR)
        pipeline.set_phase_modulator(lfo_freq_hz=lfo_freq_hz, depth_rad=depth_rad)
        print(f"[DSP Mode] 1: LFO Phase Modulator (Rate: {lfo_freq_hz:.2f} Hz, Depth: {depth_deg:.1f} deg)")
    elif mode == 2:
        pipeline.set_mode(dsp_engine.MixingMode.MODE_FREQUENCY_SHIFT)
        pipeline.set_dfs_offset(delta_f_hz=dfs_offset_hz)
        print(f"[DSP Mode] 2: Continuous Frequency Shift (SSB DFS: +{dfs_offset_hz:.2f} Hz)")

    input_device_id, extra_settings = resolve_device(target_device, loopback=loopback)

    def callback(indata, frames, time_info, status):
        if status:
            print(f"\n[Status Warning] {status}", file=sys.stderr)
        in_l = np.ascontiguousarray(indata[:, 0], dtype=np.float32)
        in_r = np.ascontiguousarray(indata[:, 1] if indata.shape[1] > 1 else indata[:, 0], dtype=np.float32)
        results = pipeline.process_frames(in_l, in_r)
        
        mid_mag = float(np.mean(np.abs(results["mid_post"])))
        side_mag = float(np.mean(np.abs(results["side_post"])))
        mid_db = 20.0 * np.log10(mid_mag + 1e-9)
        side_db = 20.0 * np.log10(side_mag + 1e-9)
        print(f"\r[Analytic RMS] Mid: {mid_db:5.1f} dBFS | Side: {side_db:5.1f} dBFS   ", end="", flush=True)

    stream_kwargs = {
        "device": input_device_id,
        "channels": 2,
        "samplerate": FS,
        "blocksize": BLOCK_SIZE,
        "dtype": 'float32',
        "callback": callback
    }
    if extra_settings is not None:
        stream_kwargs["extra_settings"] = extra_settings

    print("[Audio Stream] Streaming started. Press Ctrl+C to stop.")
    with sd.InputStream(**stream_kwargs):
        try:
            while True:
                sd.sleep(100)
        except KeyboardInterrupt:
            print("\n[Audio Stream] Stream terminated by user.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Win11 Audio C++ DSP Pipeline runner")
    parser.add_argument("--list-devices", action="store_true", help="List all available audio input/output devices")
    parser.add_argument("--mode", choices=["analyze", "stream"], default="analyze", help="Execution mode")
    parser.add_argument("--save-plot", type=str, default=None, help="Save analytical plots to an image file (e.g. analysis.png)")
    parser.add_argument("--stream-mode", type=int, choices=[0, 1, 2], default=2,
                        help="0: Phase Shift, 1: LFO Phase Modulator, 2: DFS Frequency Shift")
    parser.add_argument("--device", type=str, default=None, help="Device ID number or substring name")
    parser.add_argument("--loopback", action="store_true", help="Capture PC audio output via WASAPI Loopback")
    parser.add_argument("--phase-shift-deg", type=float, default=45.0, help="Phase shift in degrees for mode 0")
    parser.add_argument("--lfo-freq", type=float, default=1.5, help="LFO rate in Hz for mode 1")
    parser.add_argument("--mod-depth-deg", type=float, default=90.0, help="Phase modulation depth in degrees for mode 1")
    parser.add_argument("--dfs-offset", type=float, default=5.0, help="Frequency shift offset in Hz for mode 2")
    args = parser.parse_args()

    if args.list_devices:
        list_audio_devices()
    elif args.mode == "analyze":
        run_analytical_mode(save_plot=args.save_plot)
    else:
        run_streaming_mode(
            target_device=args.device,
            mode=args.stream_mode,
            loopback=args.loopback,
            phase_shift_deg=args.phase_shift_deg,
            lfo_freq_hz=args.lfo_freq,
            depth_deg=args.mod_depth_deg,
            dfs_offset_hz=args.dfs_offset
        )