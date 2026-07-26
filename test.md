# Manual testing — codec benchmark harness

How to run and verify `tools/benchmark.sh`, which puts one source file through
AetherCodec NL/HQ, FLAC, Opus and SBC and emits a comparison table plus a
side-by-side spectrogram grid.

Related tooling:
- `tools/benchmark.sh` — orchestration (deps check, encode/decode, temp files)
- `tools/benchmark_report.py` — alignment, SNR, markdown table, spectrogram grid
- `tools/aether_snr.py` — shared WAV/SNR/alignment math, also used by
  `tools/compare_spectra.py`

## Prerequisites

```bash
sudo apt install sox flac opus-tools sbc-tools
python3 -m venv .venv-bench && .venv-bench/bin/pip install numpy matplotlib
```

The system Python is PEP-668 managed and has no numpy, so the venv is not
optional. `benchmark.sh` discovers `.venv-bench/` automatically — you only need
to name it explicitly when running the Python tools by hand.

AetherCodec's own file tools must be built first (see `CLAUDE.md`):

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

The dependency check runs first on every invocation and prints the `apt install`
line for anything missing. A missing rival codec skips its row; only sox, numpy
and the aether tools are fatal.

## Test 1 — synthetic sweep (no music file needed)

```bash
sox -n -r 96000 -b 24 -c 2 /tmp/sweep.wav synth 5 sine 20:20000 vol 0.5
./tools/benchmark.sh /tmp/sweep.wav
```

Outputs `benchmark_results.md` and `benchmark_spectra.png` into the **current
directory**; set `AETHER_BENCH_OUT=/some/dir` to redirect.

Expected (5 s sweep, 48 kHz reference, 1,440,000 raw PCM bytes):

| Codec | Bitrate (kbps) | Ratio | Lossless | SNR audible | SNR full |
|---|---:|---:|:---:|---:|---:|
| AetherCodec NL | 923 | 2.50× | YES | bit-exact | bit-exact |
| FLAC (--best) | 525 | 4.39× | YES | bit-exact | bit-exact |
| SBC (bitpool 53) | 357 | 6.45× | No | 65.2 | 65.4 |
| Opus (510 kbps) | 489 | 4.71× | No | 30.8 | 23.5 |
| AetherCodec HQ | 109 | 21.06× | No | 26.8 | 20.8 |

## Test 2 — real music

Any format sox can read; it is downsampled to the 48 kHz reference for you.

```bash
sox "/path/to/track.flac" -b 24 /tmp/music.wav trim 45 15
./tools/benchmark.sh /tmp/music.wav
```

Expected shape (15 s of dense broadband music): NL and FLAC still lossless but
with ratios collapsing toward ~1.2–1.3× (a sweep is trivially predictable, real
music is not); SBC pinned at 357 kbps; HQ and Opus both landing near 480 kbps.

## What "passing" looks like

Read these four things, in order:

1. **AetherCodec NL and FLAC both report `Lossless = YES`.** If either says No,
   suspect the harness before the codec — it is nearly always a misread WAV or a
   bad alignment, not a real loss of bit-exactness. Cross-check with
   `tests/test_codec_nl`.
2. **The alignment table reads NL 0, FLAC 0, Opus 0, SBC 73, HQ 512.** These are
   physical constants, not tuning: 73 is the 8-subband SBC synthesis filterbank
   delay and 512 is `MDCT_HOP`. The harness is told none of them — it recovers
   them by cross-correlation, so if they drift, the correlator is locking onto a
   wrong peak and every SNR below is suspect.
3. **Bitrates in the right decade** — SBC always 357 at 48 kHz/bitpool 53, Opus
   ~490, NL ~900 on a sweep and ~1900 on music.
4. **Spectrogram grid**: Opus cuts hard at 20 kHz, AetherCodec HQ goes black
   above ~19 kHz (the ATH doing its job). Those are the two visibly band-limited
   panels.

## Tests that try to break it

A table that prints is not a table that is correct. These three check that it
is not quietly lying.

### Alignment is real, not decoration

```bash
./tools/benchmark.sh /tmp/sweep.wav --shift-sbc 0 --no-plot
```

SBC must collapse from ~65 dB to ~−3 dB, and its "Source" column must read
`manual`. If the number does not move, per-codec alignment is not being applied
and every SNR in the table is meaningless.

### A missing rival skips one row, never the run

```bash
sudo mv /usr/bin/sbcenc /usr/bin/sbcenc.bak
./tools/benchmark.sh /tmp/sweep.wav --no-plot   # SBC skipped, other 4 rows intact
sudo mv /usr/bin/sbcenc.bak /usr/bin/sbcenc
```

The SBC row should move to a "Skipped codecs" section with its install hint.

### Every codec really got the same input

This is the entire methodology, so verify it rather than trust it:

```bash
./tools/benchmark.sh /tmp/sweep.wav --keep-tmp --no-plot
ls -la /tmp/aether-bench.*/
```

There must be exactly one `ref48.wav`, and every encode must derive from it —
no per-codec source variants. The same directory holds each `decoded_*.wav`, so
you can play them against each other, which is the only real way to judge the
lossy rows.

### The SNR math agrees with the older tool

`compare_spectra.py` and `benchmark_report.py` share `aether_snr.py`, so they
must agree; a mismatch means one caller is passing different arguments.

```bash
REPO=~/D_drive/aethercodec
TMP=$(echo /tmp/aether-bench.*/ | head -1)
"$REPO/.venv-bench/bin/python" "$REPO/tools/compare_spectra.py" \
    "$TMP/ref48.wav" "$TMP/decoded_aether_hq.wav" HQ --shift 512 --no-plot
```

Should print the same audible/full SNR as the HQ row (26.8 / 20.8 on the sweep).

## Reading the SNR columns honestly

SNR measures *waveform* deviation. It ranks the lossless codecs correctly and is
a poor proxy for perceived quality among the lossy ones:

- **SBC scores highest of the lossy codecs. This is expected and is not a win
  for SBC.** It is a waveform-preserving subband coder that spends bits
  everywhere, which is exactly what SNR rewards. Opus and AetherCodec HQ
  deliberately discard content they judge inaudible, and SNR charges them full
  price for it.
- At 48 kHz the audible-band low-pass (cascaded 5-tap MA, from
  `tests/test_codec_hq.c`) has its first null at ~9.6 kHz — not the ~19 kHz it
  gives at 96 kHz. So the audible column weights the low/mid band and
  under-reports damage in the top octaves. The spectrogram grid covers that gap.
- The gap between the full-band and audible columns is the useful diagnostic:
  wide gap ⇒ the codec's error sits in the top octaves (SBC on music, 42.2 vs
  28.8); narrow gap ⇒ error spread evenly (Opus, 32.6 vs 31.2).
- SBC at bitpool 53 does **not** visibly roll off its top octave — it keeps it
  and gets noisier. The "SBC rolls off the highs" intuition applies at low
  bitpool, not here.

Ranking perceived quality needs listening tests (MUSHRA/ABX), which this harness
deliberately does not attempt.

## Gotchas

- Use `.venv-bench/bin/python`, not `python3`, for manual invocations of the
  Python tools — the system Python has no numpy. `benchmark.sh` handles this
  itself.
- `--keep-tmp` directories are 10–50 MB each and are not cleaned up:
  `rm -rf /tmp/aether-bench.*` when finished.
- SBC's tools are the fiddly ones — `sbcenc` reads *only* Sun/NeXT `.au`
  (S16_BE) and writes the stream to **stdout** (there is no `-o`). The harness
  converts at both ends with sox; if you drive SBC by hand, that is the trap.
- SBC measures 357 kbps, not the commonly quoted ~328. 328 is the 44.1 kHz
  figure; at 48 kHz the same bitpool 53 yields 118-byte frames × 375 frames/s.

## Out of scope

Not implemented, and not planned here:

- Live over-the-air comparison against real A2DP/LDAC/aptX-HD hardware.
- MUSHRA/ABX subjective listening scores (needs human listeners).
- Latency measurement (needs acoustic or loopback capture).

The LDAC and aptX-HD rows in the generated table are published specifications
only, labelled "(published spec, NOT measured here)". They are proprietary and
are never measured by this harness — do not present them as results.
