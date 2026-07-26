#!/usr/bin/env python3
"""aether_snr.py — shared WAV / SNR / alignment math.

Factored out of compare_spectra.py so that it and benchmark_report.py measure
quality with *one* implementation. If the low-pass or the SNR definition ever
changes, it changes here and both callers move together.

The audible-band filter is a cascaded 5-tap moving average (LP_PASSES passes),
matching tests/test_codec_hq.c. Its first null sits at fs/5 — ~19.2 kHz at
96 kHz (close to the ATH cutoff, which is why it was chosen) but ~9.6 kHz at
48 kHz. At 48 kHz it is therefore a fairly narrow "audible band": it compares
codecs over the low/mid spectrum only. That is still a fair comparison — every
codec gets the identical filter — but a codec whose damage is concentrated
above 10 kHz (SBC) will look better in this number than it sounds. Read it
alongside the spectrogram, which shows the whole band.
"""
import wave

import numpy as np

LP_PASSES = 4      # cascaded 5-tap MA, same as tests/test_codec_hq.c
WARMUP = 64        # samples skipped after filtering, as in test_codec_hq.c
LOSSLESS_DB = 120  # full-band SNR above this is bit-exact for our purposes


def lowpass(x, passes=LP_PASSES):
    """Cascaded 5-tap moving average.

    Vectorised form of the original per-sample loop: for i >= 4,
    y[i] = mean(x[i-4:i+1]); the first four samples pass through untouched.
    The WARMUP skip above discards the region where that distinction matters.
    """
    kernel = np.ones(5) / 5.0
    for _ in range(passes):
        if len(x) < 5:
            return x
        y = x.copy()
        y[4:] = np.convolve(x, kernel, mode='valid')
        x = y
    return x


def read_wav(path):
    """Return (samples as float64 in [-1,1] shaped (frames, channels), rate)."""
    with wave.open(path, 'rb') as w:
        channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())

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


def apply_shift(original, decoded, shift):
    """Align two (frames, channels) arrays by `shift` and trim to equal length.

    Positive shift = the decoder emitted `shift` extra samples up front (its
    latency), so drop them. Negative shift = the decoder ate samples from the
    start, so drop the matching head of the reference instead.
    """
    if shift > 0:
        decoded = decoded[shift:]
    elif shift < 0:
        original = original[-shift:]
    n = min(len(original), len(decoded))
    return original[:n], decoded[:n]


def find_shift(original, decoded, rate, max_lag_s=0.25, window_s=2.0):
    """Integer sample lag of `decoded` behind `original`, by peak correlation.

    Every codec has its own decode latency, and a handful of samples of offset
    collapses SNR even for a perfect codec. Cross-correlate a mono mixdown of
    the first `window_s` seconds and take the peak lag.
    """
    ref = original.mean(axis=1)
    dec = decoded.mean(axis=1)
    max_lag = int(max_lag_s * rate)
    n = min(len(ref), len(dec), int(window_s * rate))
    if n <= max_lag * 2:
        return 0

    ref = ref[:n] - ref[:n].mean()
    dec = dec[:n] - dec[:n].mean()
    if not np.any(ref) or not np.any(dec):
        return 0

    # corr[lag] = sum_i ref[i] * dec[i + lag]; maximise over |lag| <= max_lag.
    size = 1 << int(np.ceil(np.log2(2 * n)))
    corr = np.fft.irfft(np.conj(np.fft.rfft(ref, size)) * np.fft.rfft(dec, size), size)
    lags = np.concatenate([np.arange(0, max_lag + 1), np.arange(size - max_lag, size)])
    best = lags[int(np.argmax(corr[lags]))]
    return int(best if best <= max_lag else best - size)


def snr_metrics(original, decoded, shift=0):
    """Full-band and audible-band SNR of `decoded` against reference `original`."""
    original, decoded = apply_shift(original, decoded, shift)
    channels = min(original.shape[1], decoded.shape[1])
    original = original[:, :channels]
    decoded = decoded[:, :channels]

    diff = original - decoded
    rms_orig = float(np.sqrt(np.mean(original ** 2)))
    rms_diff = float(np.sqrt(np.mean(diff ** 2)))
    snr_full = 20 * np.log10(rms_orig / (rms_diff + 1e-12))

    sig_a = err_a = 0.0
    for c in range(channels):
        ref = lowpass(original[:, c].copy())
        dec = lowpass(decoded[:, c].copy())
        e = ref[WARMUP:] - dec[WARMUP:]
        sig_a += float(np.sum(ref[WARMUP:] ** 2))
        err_a += float(np.sum(e ** 2))
    snr_audible = 10 * np.log10(sig_a / (err_a + 1e-12))

    return {
        'rms_orig_db': 20 * np.log10(rms_orig + 1e-12),
        'rms_diff_db': 20 * np.log10(rms_diff + 1e-12),
        'snr_full': float(snr_full),
        'snr_audible': float(snr_audible),
        'lossless': bool(snr_full > LOSSLESS_DB),
        'samples': int(len(original)),
    }
