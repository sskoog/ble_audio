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

def parse_input_mode(mode_str):
    mode_map = {
        "mono": dsp_engine.InputMatrixMode.INPUT_MONO,
        "stereo": dsp_engine.InputMatrixMode.INPUT_STEREO,
        "ms": dsp_engine.InputMatrixMode.INPUT_MS
    }
    return mode_map.get(str(mode_str).lower(), dsp_engine.InputMatrixMode.INPUT_MS)

def parse_spatial_algo(algo_str):
    algo_map = {
        "bypass": dsp_engine.SpatialAlgorithm.SPATIAL_BYPASS,
        "0": dsp_engine.SpatialAlgorithm.SPATIAL_BYPASS,
        "basic": dsp_engine.SpatialAlgorithm.SPATIAL_BASIC,
        "1": dsp_engine.SpatialAlgorithm.SPATIAL_BASIC,
        "basic+": dsp_engine.SpatialAlgorithm.SPATIAL_BASIC_PLUS,
        "basic_plus": dsp_engine.SpatialAlgorithm.SPATIAL_BASIC_PLUS,
        "2": dsp_engine.SpatialAlgorithm.SPATIAL_BASIC_PLUS,
        "swirl": dsp_engine.SpatialAlgorithm.SPATIAL_SWIRL,
        "chorus": dsp_engine.SpatialAlgorithm.SPATIAL_SWIRL,
        "3": dsp_engine.SpatialAlgorithm.SPATIAL_SWIRL,
        "rotary": dsp_engine.SpatialAlgorithm.SPATIAL_ROTARY,
        "4": dsp_engine.SpatialAlgorithm.SPATIAL_ROTARY,
        "heterodyne": dsp_engine.SpatialAlgorithm.SPATIAL_HETERODYNE,
        "5": dsp_engine.SpatialAlgorithm.SPATIAL_HETERODYNE
    }
    return algo_map.get(str(algo_str).lower(), dsp_engine.SpatialAlgorithm.SPATIAL_BASIC)

def run_analytical_mode(save_plot=None, input_mode="ms", algo="basic", hilbert="fir",
                        hpf_cutoff=None, mix_ms=None, phase_shift_deg=0.0):
    if hpf_cutoff is None:
        hpf_cutoff = getattr(dsp_engine, "HPF_CUTOFF_HZ", 150.0)
    if mix_ms is None:
        mix_ms = getattr(dsp_engine, "MIX_MS_DEFAULT", 0.5)

    pipeline = dsp_engine.AudioDSPPipeline(float(FS), float(hpf_cutoff), float(mix_ms))
    
    in_mode_enum = parse_input_mode(input_mode)
    pipeline.set_input_mode(in_mode_enum)
    
    algo_enum = parse_spatial_algo(algo)
    pipeline.set_spatial_algorithm(algo_enum)
    
    h_type = dsp_engine.HilbertType.HILBERT_FIR if hilbert.lower() == "fir" else dsp_engine.HilbertType.HILBERT_IIR_ALLPASS
    pipeline.set_hilbert_type(h_type)
    
    pipeline.set_mix_ms(mix_ms)
    pipeline.set_phase_shift_deg(phase_shift_deg)
    
    h_name = "31-Tap FIR Linear Phase" if hilbert.lower() == "fir" else "2x3-Pole Cascaded Allpass IIR"
    print(f"[Analytical Mode] Input Matrix: {input_mode.upper()} | HPF Cutoff: {hpf_cutoff:.1f} Hz | Mix MS: {mix_ms:.2f} | Hilbert: {h_name}")

    n_samples = 8192
    stimulus_l = np.zeros(n_samples, dtype=np.float32)
    stimulus_r = np.zeros(n_samples, dtype=np.float32)
    stimulus_l[0] = 1.0
    stimulus_r[0] = 0.5

    results = pipeline.process_frames(stimulus_l, stimulus_r)

    # 1. Raw Signals & Dry Filtered (LPF / HPF)
    f, mag_raw_l, gd_raw_l = calculate_tf_and_group_delay(stimulus_l, results["raw_l"])
    _, mag_raw_r, gd_raw_r = calculate_tf_and_group_delay(stimulus_r, results["raw_r"])
    _, mag_dry_lpf, gd_dry_lpf = calculate_tf_and_group_delay(stimulus_l, results.get("dry0_lpf", results["raw_l"]))
    _, mag_dry_hpf, gd_dry_hpf = calculate_tf_and_group_delay(stimulus_l, results.get("dry0_hpf", results["raw_l"]))

    # 2. Mid & Side (Pre-Hilbert)
    mid_ref = 0.5 * (stimulus_l + stimulus_r)
    side_ref = 0.5 * (stimulus_l - stimulus_r)
    _, mag_mid_pre, gd_mid_pre = calculate_tf_and_group_delay(mid_ref, results["mid_pre"])
    _, mag_side_pre, gd_side_pre = calculate_tf_and_group_delay(side_ref, results["side_pre"])

    # 3. Post-Hilbert Analytic In-Phase ($I$)
    mid_i = np.real(results["mid_post"])
    side_i = np.real(results["side_post"])
    _, mag_mid_post, gd_mid_post = calculate_tf_and_group_delay(mid_ref, mid_i)
    _, mag_side_post, gd_side_post = calculate_tf_and_group_delay(side_ref, side_i)

    # 4. Final Reconstructed Output (HPF Wet + LPF Dry)
    _, mag_out_l, gd_out_l = calculate_tf_and_group_delay(stimulus_l, results["out_l"])
    _, mag_out_r, gd_out_r = calculate_tf_and_group_delay(stimulus_r, results["out_r"])

    # 4-Row Subplot Visualization
    fig, axes = plt.subplots(4, 2, figsize=(14, 12), sharex=True)

    # Row 1: Raw & Dry LPF / HPF
    axes[0, 0].semilogx(f, mag_raw_l, label='Raw Left', color='tab:blue')
    axes[0, 0].semilogx(f, mag_raw_r, label='Raw Right', color='tab:cyan', linestyle=':')
    axes[0, 0].semilogx(f, mag_dry_lpf, label='Dry LPF', color='tab:purple', linestyle='--')
    axes[0, 0].semilogx(f, mag_dry_hpf, label='Dry HPF', color='tab:olive', linestyle='--')
    axes[0, 0].set_ylabel('Magnitude (dB)')
    axes[0, 0].set_title('1. Raw L/R Input & Dry Filtered (LPF/HPF): Magnitude')
    axes[0, 0].grid(True, which='both', linestyle=':')
    axes[0, 0].legend()

    axes[0, 1].semilogx(f, gd_raw_l, label='Raw Left', color='tab:blue')
    axes[0, 1].semilogx(f, gd_raw_r, label='Raw Right', color='tab:cyan', linestyle=':')
    axes[0, 1].semilogx(f, gd_dry_lpf, label='Dry LPF', color='tab:purple', linestyle='--')
    axes[0, 1].semilogx(f, gd_dry_hpf, label='Dry HPF', color='tab:olive', linestyle='--')
    axes[0, 1].set_ylabel('Group Delay (ms)')
    axes[0, 1].set_title('1. Raw L/R Input & Dry Filtered (LPF/HPF): Group Delay')
    axes[0, 1].grid(True, which='both', linestyle=':')
    axes[0, 1].legend()

    # Row 2: Pre-Hilbert Conditioning (HPF & Matrixing)
    axes[1, 0].semilogx(f, mag_mid_pre, label='Matrix Dry0 (Mid)', color='tab:green')
    axes[1, 0].semilogx(f, mag_side_pre, label='Matrix Dry1 (Side)', color='tab:olive', linestyle='--')
    axes[1, 0].axvline(hpf_cutoff, color='r', linestyle=':', label=f'{hpf_cutoff:.0f} Hz Crossover')
    axes[1, 0].set_ylabel('Magnitude (dB)')
    axes[1, 0].set_title(f'2. Input Matrix Pre-Conditioning: {hpf_cutoff:.0f} Hz Crossover')
    axes[1, 0].grid(True, which='both', linestyle=':')
    axes[1, 0].legend()

    axes[1, 1].semilogx(f, gd_mid_pre, label='Matrix Dry0 (Mid)', color='tab:green')
    axes[1, 1].semilogx(f, gd_side_pre, label='Matrix Dry1 (Side)', color='tab:olive', linestyle='--')
    axes[1, 1].set_ylabel('Group Delay (ms)')
    axes[1, 1].set_title('2. Input Matrix: Phase Delay')
    axes[1, 1].grid(True, which='both', linestyle=':')
    axes[1, 1].legend()

    # Row 3: Post-Hilbert Analytic In-Phase ($I$)
    axes[2, 0].semilogx(f, mag_mid_post, label='Mid Analytic (I)', color='tab:red')
    axes[2, 0].semilogx(f, mag_side_post, label='Side Analytic (I)', color='tab:purple', linestyle='--')
    axes[2, 0].set_ylabel('Magnitude (dB)')
    axes[2, 0].set_title(f'3. Post-Hilbert Analytic Path ({h_name})')
    axes[2, 0].grid(True, which='both', linestyle=':')
    axes[2, 0].legend()

    axes[2, 1].semilogx(f, gd_mid_post, label='Mid Analytic (I)', color='tab:red')
    axes[2, 1].semilogx(f, gd_side_post, label='Side Analytic (I)', color='tab:purple', linestyle='--')
    if hilbert.lower() == "fir":
        axes[2, 1].axhline(15.0 / FS * 1000.0, color='gray', linestyle=':', label='Matched FIR Delay (0.31 ms)')
        axes[2, 1].set_title('3. Post-Hilbert: Linear Phase FIR Group Delay')
    else:
        axes[2, 1].set_title('3. Post-Hilbert: Cascaded Allpass IIR Group Delay Dispersion')
    axes[2, 1].set_ylabel('Group Delay (ms)')
    axes[2, 1].grid(True, which='both', linestyle=':')
    axes[2, 1].legend()

    # Row 4: Final Output (LPF Bypass + Wet HPF)
    axes[3, 0].semilogx(f, mag_out_l, label='Out Left (Ch 0)', color='tab:blue')
    axes[3, 0].semilogx(f, mag_out_r, label='Out Right (Ch 1)', color='tab:orange', linestyle='--')
    axes[3, 0].set_xlabel('Frequency (Hz)')
    axes[3, 0].set_ylabel('Magnitude (dB)')
    axes[3, 0].set_title('4. Final Reconstructed Output: Full Spectrum (LPF + Wet HPF)')
    axes[3, 0].grid(True, which='both', linestyle=':')
    axes[3, 0].legend()

    axes[3, 1].semilogx(f, gd_out_l, label='Out Left (Ch 0)', color='tab:blue')
    axes[3, 1].semilogx(f, gd_out_r, label='Out Right (Ch 1)', color='tab:orange', linestyle='--')
    axes[3, 1].set_xlabel('Frequency (Hz)')
    axes[3, 1].set_ylabel('Group Delay (ms)')
    axes[3, 1].set_title('4. Final Reconstructed Output: Group Delay')
    axes[3, 1].grid(True, which='both', linestyle=':')
    axes[3, 1].legend()

    # Set y-axis limits for all dB magnitude plots
    for row in range(4):
        axes[row, 0].set_ylim(-70.0, 6.0)

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

def run_streaming_mode(target_device=None, input_mode="ms", algo="swirl", hilbert="fir",
                       num_channels=2, mix_ms=None, hpf_cutoff=None, loopback=False,
                       phase_shift_deg=45.0, dfs_offset_hz=5.0, dfs_step_hz=0.4,
                       rotary_scale=1.0, rotary_depth_deg=90.0):
    if hpf_cutoff is None:
        hpf_cutoff = getattr(dsp_engine, "HPF_CUTOFF_HZ", 150.0)
    if mix_ms is None:
        mix_ms = getattr(dsp_engine, "MIX_MS_DEFAULT", 0.5)

    pipeline = dsp_engine.AudioDSPPipeline(float(FS), float(hpf_cutoff), float(mix_ms))
    
    in_mode_enum = parse_input_mode(input_mode)
    pipeline.set_input_mode(in_mode_enum)
    
    algo_enum = parse_spatial_algo(algo)
    pipeline.set_spatial_algorithm(algo_enum)
    
    h_type = dsp_engine.HilbertType.HILBERT_FIR if hilbert.lower() == "fir" else dsp_engine.HilbertType.HILBERT_IIR_ALLPASS
    pipeline.set_hilbert_type(h_type)
    
    pipeline.set_num_channels(num_channels)
    pipeline.set_mix_ms(mix_ms)
    pipeline.set_phase_shift_deg(phase_shift_deg)
    pipeline.set_dfs_offset(dfs_offset_hz)
    pipeline.set_dfs_step(dfs_step_hz)
    pipeline.set_rotary_params(rotary_scale, rotary_depth_deg)
    
    h_name = "31-Tap FIR Linear Phase" if hilbert.lower() == "fir" else "2x3-Pole Cascaded Allpass IIR"
    print(f"[Audio Stream] Input Matrix: {input_mode.upper()} | Spatial Algo: {algo.upper()} | Output Channels: {num_channels}")
    print(f"[Audio Stream] HPF Cutoff: {hpf_cutoff:.1f} Hz | Mix MS: {mix_ms:.2f} | Hilbert: {h_name}")

    input_device_id, extra_settings = resolve_device(target_device, loopback=loopback)

    def callback(indata, frames, time_info, status):
        if status:
            print(f"\n[Status Warning] {status}", file=sys.stderr)
        in_l = np.ascontiguousarray(indata[:, 0], dtype=np.float32)
        in_r = np.ascontiguousarray(indata[:, 1] if indata.shape[1] > 1 else indata[:, 0], dtype=np.float32)
        results = pipeline.process_frames(in_l, in_r)
        
        # Monitor RMS of output channels
        out_ch = results["out_channels"]
        rms_strs = []
        for ch in range(min(num_channels, 4)):
            ch_rms = float(np.sqrt(np.mean(out_ch[ch]**2)))
            ch_db = 20.0 * np.log10(ch_rms + 1e-9)
            rms_strs.append(f"Ch{ch}: {ch_db:5.1f} dBFS")
        
        print("\r[RMS Levels] " + " | ".join(rms_strs) + "   ", end="", flush=True)

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
    parser = argparse.ArgumentParser(description="Win11 Audio Spatial DSP Pipeline runner")
    parser.add_argument("--list-devices", action="store_true", help="List all available audio input/output devices")
    parser.add_argument("--mode", choices=["analyze", "stream"], default="analyze", help="Execution mode")
    parser.add_argument("--input-mode", choices=["mono", "stereo", "ms"], default="ms",
                        help="Input mixing passive matrix: mono, stereo, or ms (mid-side)")
    parser.add_argument("--algo", choices=["bypass", "basic", "basic+", "swirl", "rotary", "heterodyne", "0", "1", "2", "3", "4", "5"],
                        default="swirl", help="Spatial audio algorithm (0: Bypass, 1: Basic, 2: Basic+, 3: Swirl, 4: Rotary, 5: Heterodyne)")
    parser.add_argument("--hilbert", choices=["fir", "iir"], default="fir",
                        help="Hilbert transformer type: 'fir' (31-tap linear-phase FIR) or 'iir' (low-weight 2x3-pole cascaded allpass IIR)")
    parser.add_argument("--channels", type=int, default=2, help="Number of output audio channels (e.g. 2 for stereo, 4, 5, or 8)")
    parser.add_argument("--mix-ms", type=float, default=getattr(dsp_engine, "MIX_MS_DEFAULT", 0.5),
                        help="Mixing factor between center anchor and wet side channel (0.0 to 1.0, default from C++ MIX_MS_DEFAULT)")
    parser.add_argument("--hpf-cutoff", type=float, default=getattr(dsp_engine, "HPF_CUTOFF_HZ", 150.0),
                        help="High-pass filter crossover frequency in Hz")
    parser.add_argument("--save-plot", type=str, default=None, help="Save analytical plots to an image file (e.g. analysis.png)")
    parser.add_argument("--device", type=str, default=None, help="Device ID number or substring name")
    parser.add_argument("--loopback", action="store_true", help="Capture PC audio output via WASAPI Loopback")
    parser.add_argument("--phase-shift-deg", type=float, default=45.0, help="Static phase shift angle in degrees")
    parser.add_argument("--dfs-offset", type=float, default=5.0, help="Frequency shift offset in Hz for basic+ and DFS")
    parser.add_argument("--dfs-step", type=float, default=0.4, help="Fractional DFS step in Hz across channels for Heterodyning")
    parser.add_argument("--rotary-scale", type=float, default=1.0, help="Speed multiplier for prime LFO rotary rates")
    parser.add_argument("--rotary-depth-deg", type=float, default=90.0, help="Rotary phase modulation depth in degrees")
    
    # Backwards-compatible aliases
    parser.add_argument("--stream-mode", type=int, default=None, help="Legacy stream mode alias")
    parser.add_argument("--lfo-freq", type=float, default=None, help="Legacy LFO rate alias")
    parser.add_argument("--mod-depth-deg", type=float, default=None, help="Legacy modulation depth alias")
    
    args = parser.parse_args()

    # Map legacy arguments if provided
    if args.stream_mode is not None:
        mode_map = {0: "basic", 1: "rotary", 2: "basic+"}
        args.algo = mode_map.get(args.stream_mode, args.algo)
    if args.lfo_freq is not None:
        args.rotary_scale = args.lfo_freq
    if args.mod_depth_deg is not None:
        args.rotary_depth_deg = args.mod_depth_deg

    if args.list_devices:
        list_audio_devices()
    elif args.mode == "analyze":
        run_analytical_mode(
            save_plot=args.save_plot,
            input_mode=args.input_mode,
            algo=args.algo,
            hilbert=args.hilbert,
            hpf_cutoff=args.hpf_cutoff,
            mix_ms=args.mix_ms,
            phase_shift_deg=args.phase_shift_deg
        )
    else:
        run_streaming_mode(
            target_device=args.device,
            input_mode=args.input_mode,
            algo=args.algo,
            hilbert=args.hilbert,
            num_channels=args.channels,
            mix_ms=args.mix_ms,
            hpf_cutoff=args.hpf_cutoff,
            loopback=args.loopback,
            phase_shift_deg=args.phase_shift_deg,
            dfs_offset_hz=args.dfs_offset,
            dfs_step_hz=args.dfs_step,
            rotary_scale=args.rotary_scale,
            rotary_depth_deg=args.rotary_depth_deg
        )