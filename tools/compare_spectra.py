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
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aether_snr import read_wav, snr_metrics   # noqa: E402  shared with benchmark_report.py


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

    m = snr_metrics(original, decoded, shift)
    snr_full = m['snr_full']

    # Re-derive the aligned pair for the plots below (snr_metrics does its own).
    decoded = decoded[shift:]
    n = min(len(original), len(decoded))
    original = original[:n]
    decoded  = decoded[:n]
    diff = original - decoded

    print(f"Comparing: {orig_path}  vs  {dec_path}  ({label})")
    print(f"  Original RMS:     {m['rms_orig_db']:6.1f} dBFS")
    print(f"  Difference RMS:   {m['rms_diff_db']:6.1f} dBFS")
    print(f"  SNR full-band:    {snr_full:6.1f} dB")
    print(f"  SNR audible-band: {m['snr_audible']:6.1f} dB")
    print(f"  Lossless (>120dB full-band): {'YES' if m['lossless'] else 'NO'}")

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
