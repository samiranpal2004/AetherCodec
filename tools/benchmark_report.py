#!/usr/bin/env python3
"""benchmark_report.py — table + spectrogram grid for tools/benchmark.sh.

Reads the JSON manifest benchmark.sh writes (one entry per codec that actually
ran: encoded size, bitrate, decoded WAV path), measures every decoded file
against the SAME 48 kHz reference, and emits:

  1. a markdown table on stdout and in benchmark_results.md
  2. benchmark_spectra.png — one spectrogram panel per codec, shared frequency
     axis and shared dB colour scale, so the panels are directly comparable

Alignment is auto-detected per codec by cross-correlation (see aether_snr.py);
--shift-<codec> overrides it. SNR math is shared with compare_spectra.py.

Usage:
    benchmark_report.py <manifest.json> [--out-dir DIR] [--no-plot]
                        [--shift-<codec> N ...]
"""
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aether_snr import find_shift, read_wav, snr_metrics   # noqa: E402

# Published specifications, for context only. These are NOT measured by this
# harness -- neither codec is open source and we have no hardware to run them.
REFERENCE_ROWS = [
    ("LDAC (990 kbps)", 990.0, "No"),
    ("aptX-HD (576 kbps)", 576.0, "No"),
]
SPEC_NOTE = "(published spec, NOT measured here)"


def measure(manifest, overrides):
    ref, ref_rate = read_wav(manifest['reference'])
    raw_bytes = manifest['raw_ref_bytes']
    rows = []

    for entry in manifest['codecs']:
        key, label = entry['key'], entry['label']
        try:
            dec, dec_rate = read_wav(entry['decoded'])
        except Exception as exc:                       # noqa: BLE001
            print(f"warning: {label}: cannot read decoded WAV ({exc}) — skipping",
                  file=sys.stderr)
            continue
        if dec_rate != ref_rate:
            print(f"warning: {label}: decoded at {dec_rate} Hz, reference is "
                  f"{ref_rate} Hz — skipping", file=sys.stderr)
            continue

        if key in overrides:
            shift, how = overrides[key], "manual"
        else:
            shift, how = find_shift(ref, dec, ref_rate), "auto"

        m = snr_metrics(ref, dec, shift)
        m.update(
            key=key, label=label, shift=shift, shift_how=how,
            bitrate_kbps=entry['bitrate_kbps'],
            ratio=raw_bytes / entry['bytes'] if entry['bytes'] else float('nan'),
            decoded=entry['decoded'], note=entry.get('note', ''),
        )
        rows.append(m)

    # Lossless first, then lossy by descending audible-band SNR.
    rows.sort(key=lambda r: (not r['lossless'], -r['snr_audible']))
    return ref, ref_rate, rows


def render_table(manifest, rows, skipped):
    out = []
    out.append("# AetherCodec codec benchmark\n")
    out.append(f"Source: `{manifest['source']}`  ")
    out.append(f"Reference (ground truth for every row): `{manifest['reference']}` — "
               f"{manifest['rate']} Hz / {manifest['bits']}-bit / "
               f"{manifest['channels']} ch, {manifest['duration_s']:.2f} s, "
               f"{manifest['raw_ref_bytes']:,} raw PCM bytes\n")
    out.append("Every codec encodes that identical file; every SNR is measured "
               "against it, never codec-vs-codec. Per-codec decode latency is "
               "removed by cross-correlation before measuring.\n")
    out.append("**Read the audible-band SNR column** — full-band unfairly "
               "punishes codecs that correctly discard content above hearing.\n")

    out.append("| Codec | Bitrate (kbps) | Compression ratio | Lossless | "
               "SNR audible (dB) | SNR full (dB) |")
    out.append("|---|---:|---:|:---:|---:|---:|")
    for r in rows:
        loss = "**YES**" if r['lossless'] else "No"
        aud = "∞ (bit-exact)" if r['lossless'] else f"{r['snr_audible']:.1f}"
        full = "∞ (bit-exact)" if r['lossless'] else f"{r['snr_full']:.1f}"
        out.append(f"| {r['label']} | {r['bitrate_kbps']:.0f} | {r['ratio']:.2f}× | "
                   f"{loss} | {aud} | {full} |")
    for label, kbps, loss in REFERENCE_ROWS:
        out.append(f"| {label} {SPEC_NOTE} | {kbps:.0f} | — | {loss} | — | — |")
    out.append("")

    if skipped:
        out.append("## Skipped codecs\n")
        for s in skipped:
            out.append(f"- **{s['label']}** — {s['reason']}")
        out.append("")

    out.append("## Alignment applied\n")
    out.append("| Codec | Shift (samples) | Source |")
    out.append("|---|---:|---|")
    for r in rows:
        out.append(f"| {r['label']} | {r['shift']} | {r['shift_how']} |")
    out.append("")

    out.append("## How to read the SNR columns\n")
    out.append("SNR measures *waveform* deviation, so it ranks lossless codecs "
               "correctly but is a poor proxy for perceived quality among the "
               "lossy ones. A subband coder like SBC tracks the waveform and "
               "spends bits everywhere, so it scores well on SNR. Perceptual "
               "coders — Opus and AetherCodec HQ — deliberately discard content "
               "they judge inaudible and do not preserve waveform above a few "
               "kHz, which SNR charges them full price for. **A higher SNR row "
               "here does not mean it sounds better**; ranking perceived quality "
               "needs listening tests (MUSHRA/ABX), which this harness does not "
               "do.\n")
    out.append("The gap between the full-band and audible-band columns is the "
               "useful diagnostic: a large gap means the codec's error is "
               "concentrated above the low-pass, i.e. in the top octaves.\n")

    out.append("## Notes\n")
    out.append("- Lossless is declared at full-band SNR > 120 dB.")
    out.append("- The audible-band low-pass is the cascaded 5-tap moving average "
               "from `tests/test_codec_hq.c`; at 48 kHz its first null is ~9.6 kHz, "
               "so this column weights the low/mid band. Damage concentrated in the "
               "top octaves shows up in the spectrogram grid rather than in this "
               "number.")
    out.append(f"- LDAC and aptX-HD rows are {SPEC_NOTE.strip('()')}; they are "
               "proprietary and cannot be run here.")
    return "\n".join(out) + "\n"


def render_grid(ref, rate, rows, out_png):
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except ImportError:
        print("(matplotlib not installed — skipping spectrogram grid; "
              "`pip install matplotlib`)")
        return False

    panels = [("Reference (48 kHz original)", ref[:, 0], None)]
    for r in rows:
        dec, _ = read_wav(r['decoded'])
        panels.append((r['label'], dec[:, 0], r))

    # One dB colour scale for every panel, anchored on the reference, or the
    # comparison is meaningless.
    spec_kw = dict(Fs=rate, NFFT=2048, noverlap=1024)
    fig_probe = plt.figure()
    pxx, _, _, _ = plt.specgram(ref[:, 0], **spec_kw)
    plt.close(fig_probe)
    ref_db = 10 * np.log10(pxx + 1e-20)
    vmax = float(np.percentile(ref_db, 99.9))
    vmin = vmax - 100.0

    ncols = min(3, len(panels))
    nrows = (len(panels) + ncols - 1) // ncols
    fig, axes = plt.subplots(nrows, ncols, figsize=(5.4 * ncols, 3.6 * nrows + 1.0),
                             squeeze=False)
    flat = [ax for row in axes for ax in row]

    im = None
    for ax, (title, sig, r) in zip(flat, panels):
        _, _, _, im = ax.specgram(sig, vmin=vmin, vmax=vmax, cmap='magma', **spec_kw)
        ax.set_ylim(0, rate / 2)               # identical frequency axis everywhere
        ax.set_title(title if r is None
                     else f"{title} — audible SNR "
                          + ("bit-exact" if r['lossless'] else f"{r['snr_audible']:.1f} dB"),
                     fontsize=10)
        ax.set_ylabel("Freq (Hz)")
        ax.set_xlabel("Time (s)")
    for ax in flat[len(panels):]:
        ax.axis('off')

    fig.suptitle("AetherCodec vs FLAC / Opus / SBC — identical 48 kHz source, "
                 "shared frequency and dB scale", fontsize=14)
    # Reserve a strip at the bottom for the shared colour bar rather than letting
    # a colorbar attached to every axis eat into the panels.
    fig.tight_layout(rect=(0, 0.14, 1, 0.95))
    if im is not None:
        cax = fig.add_axes((0.30, 0.075, 0.40, 0.015))
        cbar = fig.colorbar(im, cax=cax, orientation='horizontal')
        cbar.set_label("Power (dB, shared scale across all panels)")
    fig.savefig(out_png, dpi=140)
    plt.close(fig)
    return True


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)

    manifest_path = args[0]
    out_dir = "."
    no_plot = False
    overrides = {}
    i = 1
    while i < len(args):
        a = args[i]
        if a == '--out-dir':
            out_dir = args[i + 1]; i += 2
        elif a == '--no-plot':
            no_plot = True; i += 1
        elif a.startswith('--shift-'):
            overrides[a[len('--shift-'):]] = int(args[i + 1]); i += 2
        else:
            print(f"unknown argument: {a}"); sys.exit(1)

    with open(manifest_path) as f:
        manifest = json.load(f)

    ref, rate, rows = measure(manifest, overrides)
    if not rows:
        print("No codec produced a measurable output.", file=sys.stderr)
        sys.exit(1)

    table = render_table(manifest, rows, manifest.get('skipped', []))
    print(table)

    md_path = os.path.join(out_dir, "benchmark_results.md")
    with open(md_path, 'w') as f:
        f.write(table)
    print(f"Wrote {md_path}")

    if not no_plot:
        png_path = os.path.join(out_dir, "benchmark_spectra.png")
        if render_grid(ref, rate, rows, png_path):
            print(f"Wrote {png_path}")


if __name__ == '__main__':
    main()
