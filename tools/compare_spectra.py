#!/usr/bin/env python3
"""compare_spectra.py — Phase 6 quality measurement.

Compares an original WAV against AetherCodec's decoded output (from
aether_decode): full-band SNR, audible-band SNR, and a spectrogram PNG.

Two SNRs are reported for the same reason tests/test_codec_hq.c reports two:
at 96 kHz most of the spectrum sits above the absolute threshold of hearing,
where HQ mode correctly quantises everything to zero. A full-band SNR
penalises that correct behaviour; the audible-band figure (same low-pass
applied to both signals) is the meaningful one. NL mode should show a huge
full-band SNR too (it's bit-exact), so the two numbers converge there.

Usage:
    python3 tools/compare_spectra.py original.wav decoded.wav [label] [--shift N] [--no-plot]

    --shift N   Skip N samples at the start of the decoded file before
                aligning with the original. HQ mode has an inherent one-hop
                (512 samples @ 96kHz, matching MDCT_HOP) OLA delay — pass
                --shift 512 when comparing HQ output. NL mode needs no shift.
"""
import sys
import wave
import numpy as np

LP_PASSES = 4   # cascaded 5-tap MA, same as tests/test_codec_hq.c — first
                # null ~fs/5 (~19.2 kHz @ 96 kHz), close to the ATH cutoff.


def lowpass(x, passes):
    for _ in range(passes):
        y = x.copy()
        for i in range(4, len(x)):
            y[i] = (x[i] + x[i-1] + x[i-2] + x[i-3] + x[i-4]) / 5.0
        x = y
    return x


def read_wav(path):
    with wave.open(path, 'rb') as w:
        channels  = w.getnchannels()
        sampwidth = w.getsampwidth()
        rate      = w.getframerate()
        nframes   = w.getnframes()
        raw       = w.readframes(nframes)

    if sampwidth == 2:
        data = np.frombuffer(raw, dtype='<i2').astype(np.float64) / 32768.0
    elif sampwidth == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        data = v.astype(np.float64) / 8388608.0
    elif sampwidth == 4:
        data = np.frombuffer(raw, dtype='<i4').astype(np.float64) / 2147483648.0
    else:
        raise ValueError(f"{path}: unsupported sample width {sampwidth} bytes")

    return data.reshape(-1, channels), rate


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    args = sys.argv[1:]
    orig_path, dec_path = args[0], args[1]
    rest = args[2:]
    label = "AetherCodec"
    shift = 0
    no_plot = False
    i = 0
    while i < len(rest):
        a = rest[i]
        if a == '--shift':
            shift = int(rest[i + 1]); i += 2
        elif a == '--no-plot':
            no_plot = True; i += 1
        else:
            label = a; i += 1

    original, rate_o = read_wav(orig_path)
    decoded, rate_d = read_wav(dec_path)
    if decoded.shape[1] != original.shape[1]:
        print(f"warning: channel count differs ({original.shape[1]} vs {decoded.shape[1]})")

    decoded = decoded[shift:]
    n = min(len(original), len(decoded))
    original = original[:n]
    decoded  = decoded[:n]
    channels = original.shape[1]

    diff = original - decoded
    rms_orig = np.sqrt(np.mean(original ** 2))
    rms_diff = np.sqrt(np.mean(diff ** 2))
    snr_full = 20 * np.log10(rms_orig / (rms_diff + 1e-12))

    sig_a = err_a = 0.0
    for c in range(channels):
        ref = lowpass(original[:, c].copy(), LP_PASSES)
        dec = lowpass(decoded[:, c].copy(), LP_PASSES)
        e = ref[64:] - dec[64:]              # skip filter warm-up, as in test_codec_hq.c
        sig_a += np.sum(ref[64:] ** 2)
        err_a += np.sum(e ** 2)
    snr_audible = 10 * np.log10(sig_a / (err_a + 1e-12))

    print(f"Comparing: {orig_path}  vs  {dec_path}  ({label})")
    print(f"  Original RMS:     {20*np.log10(rms_orig+1e-12):6.1f} dBFS")
    print(f"  Difference RMS:   {20*np.log10(rms_diff+1e-12):6.1f} dBFS")
    print(f"  SNR full-band:    {snr_full:6.1f} dB")
    print(f"  SNR audible-band: {snr_audible:6.1f} dB")
    print(f"  Lossless (>120dB full-band): {'YES' if snr_full > 120 else 'NO'}")

    if not no_plot:
        try:
            import matplotlib
            matplotlib.use('Agg')
            import matplotlib.pyplot as plt

            fig, axes = plt.subplots(3, 1, figsize=(12, 9))
            axes[0].specgram(original[:, 0], Fs=rate_o, NFFT=4096, noverlap=2048)
            axes[0].set_title("Original"); axes[0].set_ylabel("Freq (Hz)")
            axes[1].specgram(decoded[:, 0], Fs=rate_d, NFFT=4096, noverlap=2048)
            axes[1].set_title(label); axes[1].set_ylabel("Freq (Hz)")
            axes[2].specgram(diff[:, 0], Fs=rate_o, NFFT=4096, noverlap=2048)
            axes[2].set_title(f"Difference (SNR full: {snr_full:.1f} dB)")
            axes[2].set_ylabel("Freq (Hz)"); axes[2].set_xlabel("Time (s)")
            plt.tight_layout()
            out_png = "spectra_comparison.png"
            plt.savefig(out_png, dpi=150)
            print(f"  Saved: {out_png}")
        except ImportError:
            print("  (matplotlib not installed — skipping spectrogram PNG; "
                 "pass --no-plot to silence this, or `pip install matplotlib`)")


if __name__ == '__main__':
    main()
