# AetherCodec — Manual Testing Guide

A short, step-by-step guide to build AetherCodec and manually verify each phase
that is currently implemented (**Phase 0–6**).

> **Legend**
> - 🖥️ **A** = Laptop A (sender)   🎧 **B** = Laptop B (receiver)
> - 💻 = runs on a single machine (no second laptop needed)

---

## 1. Build (💻)

```bash
cd ~/aethercodec
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DAETHER_ENABLE_PIPEWIRE=ON \
         -DAETHER_ENABLE_ALSA=OFF
make -j$(nproc)
```

**Expect:** builds with **no warnings or errors**, producing `src/aether_sender`
and `src/aether_receiver` alongside the tools and tests. ALSA stays OFF (the
fallback plugin is not implemented). Building with
`-DAETHER_ENABLE_PIPEWIRE=OFF` still works and simply omits the two daemons.

---

## 2. Unit tests (💻)

```bash
cd ~/aethercodec/build
ctest --output-on-failure
```

**Expect:** `100% tests passed, 0 tests failed out of 9`.

Run them individually to see the detail:

```bash
./tests/test_packet     # packet pack/unpack + CRC corruption detection
./tests/test_lpc        # LPC + Rice bit-perfect round-trip (sine / noise / silence)
./tests/test_codec_nl   # NL: full encoder → packet → decoder stereo round-trip
./tests/test_mdct       # MDCT window + perfect-reconstruction check
./tests/test_codec_hq   # HQ: full encode → decode, SNR + bitrate report
./tests/test_ring       # lock-free SPSC ring, incl. threaded producer/consumer
./tests/test_jitter     # jitter buffer: reorder, duplicate, loss detection
./tests/test_abr        # ABR ladder: classification + hysteresis timing
./tests/test_resample   # 2:1 decimate/interpolate for the 48 kHz states
```

**Expect:** every NL line reports `LOSSLESS (0 mismatches)`. Any mismatch there
is a **hard failure** — near-lossless mode must be bit-perfect. HQ is lossy by
design, so it reports SNR/bitrate instead (see §5).

---

## 3. Phase 1 — RFCOMM transport (needs A + B)

> Requires the two laptops **paired and trusted** (see Implementation Plan
> Steps 0.2–0.3). Get Laptop B's address with `hciconfig hci0 | grep "BD Address"`.
> Below, replace `BB:BB:BB:BB:BB:BB` with **Laptop B's** address.

### 3.1 Connectivity — `rfcomm_ping`

```bash
# 🎧 B  (start first — it listens)
./tools/rfcomm_ping server

# 🖥️ A
./tools/rfcomm_ping client BB:BB:BB:BB:BB:BB
```

**Expect:** B prints `Got seq=0 payload='ping 0'` … through `seq=9`.

### 3.2 Throughput — `rfcomm_bench`

```bash
# 🎧 B
./tools/rfcomm_bench server

# 🖥️ A
./tools/rfcomm_bench client BB:BB:BB:BB:BB:BB
```

**Expect:** B prints a throughput line. **Record the number** — it sets the
realistic bitrate ceiling for the codec.
- `≥ 1000 kbps` → great, full 24/96 NL mode is feasible.
- `600–1000 kbps` → OK, expect NL-48k / HQ fallback under load.
- `< 600 kbps` → BT chip likely lacks EDR; lower the target bitrate.

### 3.3 Raw PCM audio — `raw_stream_*`

Proves audio flows end-to-end **before** any codec. Needs a test file on A.

```bash
# 🎧 B  (pipe received PCM straight to the speakers)
./tools/raw_stream_receiver | aplay -r 96000 -f S16_LE -c 2

# 🖥️ A  (decode any file to raw PCM and stream it)
sox test.flac -t raw -r 96000 -b 16 -c 2 -e signed - \
  | ./tools/raw_stream_sender BB:BB:BB:BB:BB:BB
```

**Expect:** audible audio on B's headphones. It may crackle/drop at 96 kHz raw —
that's expected (raw PCM is ~3 Mbps, above the BT ceiling) and is exactly what
the codec exists to fix.

---

## 4. Phase 2 — Near-Lossless codec (💻)

The codec is fully validated on one machine by `test_lpc` and `test_codec_nl`
(section 2). To sanity-check compression on **real music**, Phase 6 adds a
dedicated file-to-file tool — see §8.1 for `aether_encode` / `aether_decode`
and `compare_spectra.py`.

Quick reasonableness check on the numbers you'll see:

| Signal   | Ratio    | Meaning                                        |
|----------|----------|------------------------------------------------|
| silence  | ~24x     | trivially compressible                         |
| sine     | ~1.4x    | pure tone (poor proxy for music)               |
| noise    | ~1.0x    | incompressible — must still be **bit-perfect** |
| music    | ~2–3x    | (measurable once WAV tools exist)              |

Over RFCOMM to Laptop B: the NL encoder plugs into the same transport as
section 3, so once the sender/receiver daemons exist (Phase 4) the codec will
ride the pipe you already verified in 3.1–3.3.

---

## 5. Phase 3 — Perceptual HQ codec (💻)

HQ mode is **lossy on purpose**, so "0 mismatches" does not apply. Two tests
cover it.

### 5.1 Transform correctness — `test_mdct`

```bash
./tests/test_mdct
```

**Expect:**
- `Princen-Bradley max deviation` **< 1e-6** — the KBD window is valid. If this
  fails, overlap-add can never reconstruct and everything downstream is noise.
- `MDCT/IMDCT overlap-add max reconstruction error` **< 1e-5** (currently
  ~4e-07). This is the Phase 3 checkpoint number.
- Bark table spans all 512 bins, non-decreasing.

> A single MDCT frame does **not** invert to its input — the IMDCT of one frame
> is time-domain *aliased* by design. Reconstruction only becomes exact after
> windowed overlap-add of successive frames, which is what the test does.

### 5.2 Codec quality & bitrate — `test_codec_hq`

```bash
./tests/test_codec_hq
```

**Expect** two rows, tonal and broadband. Reference numbers:

| Signal    | SNR full | SNR audible | Bitrate   |
|-----------|----------|-------------|-----------|
| tonal     | ~27.5 dB | ~27.5 dB    | ~540 kbps |
| broadband | ~13 dB   | ~27.6 dB    | ~1050 kbps |

How to read this:

- **Bitrate is content-adaptive.** Sparse tonal material needs far fewer bits
  than dense broadband material. Broadband (~1050 kbps) is the realistic worst
  case and lands inside the PRD's **900–1,100 kbps** HQ target.
- **Two SNRs are reported on purpose.** At 96 kHz the spectrum runs to 48 kHz,
  but the absolute threshold of hearing correctly quantises everything above
  ~17.6 kHz to zero. That discarded ultrasonic content is what drags *full-band*
  SNR down to ~13 dB on broadband. The **audible-band** figure (same low-pass
  applied to reference and decoded) is the meaningful one — and it holds ~27.5 dB
  for both signal types.
- A broken transform or quantiser collapses audible SNR far below 20 dB; that is
  what the test asserts on.

### 5.3 Tuning quality vs bitrate

The single knob is `MDCT_SMR_DB` in `src/encoder/codec_mdct_enc.c`
(signal-to-mask ratio, currently **30 dB**). Roughly **6 dB ≈ 1 bit** per
significant coefficient:

- **Lower** it (e.g. 24) → smaller packets, lower SNR.
- **Raise** it (e.g. 36) → better SNR, higher bitrate.

Rebuild and re-run `test_codec_hq` to see the new operating point. The doc's
suggested 12 dB gives only ~13 dB SNR at ~490 kbps — under half the HQ budget.

---

## 6. Phase 4 — PipeWire OS integration

> **Architecture note.** The implementation plan contradicts itself: Step 4.1
> puts the virtual sink on Laptop B, while Step 4.4 plays into
> `aether_codec_sink` on Laptop A. Only one arrangement matches the product
> goal, and it is the one implemented:
>
> | | Role |
> |---|---|
> | 🖥️ **A** (sender) | registers the virtual **sink** "AetherCodec Hi-Res BT"; apps play into it → capture → encode → RFCOMM |
> | 🎧 **B** (receiver) | RFCOMM → jitter buffer → decode → **playback** to the 3.5mm jack |

### 6.1 Sink registration (💻)

```bash
./src/aether_sender --loopback --no-play
```

In another terminal:

```bash
pw-play --list-targets            # our node should be listed
pw-cli ls Node | grep -i aether   # node.name / media.class
```

**Expect** a node with:
```
node.name        = "aether_codec_sink"
node.description = "AetherCodec Hi-Res BT"
media.class      = "Audio/Sink"
```
That is the entry a user selects in their sound settings.

### 6.2 Full audio path without Bluetooth — `--loopback` (💻)

`--loopback` runs **capture → encode → decode → playback** on one machine, so
the whole Phase 4 pipeline is testable without Laptop B.

```bash
# terminal 1 — start the sink
./src/aether_sender --loopback --mode nl --verbose

# terminal 2 — play anything into it
sox -n -r 96000 -b 24 -c 2 /tmp/tone.wav synth 6 sine 440 vol 0.5
pw-play /tmp/tone.wav                      # if our sink is the default
# or target it explicitly by the numeric id from --list-targets:
pw-play --target <ID> /tmp/tone.wav
```

**Expect** the sender's stats to advance, e.g.:

```
[pw_sink] paused -> streaming
[stats] frames=100  2.1s  376 kbps
[stats] frames=400  8.5s  2364 kbps
```

`frames` climbing is the proof that audio reached the encoder. Try
`--mode hq` too (a pure sine is sparse, so HQ settles around ~600 kbps).

> **`--no-play`** omits the playback stream. Use it when the machine has **no
> real audio output**: our virtual sink then becomes the *default* output, and
> the loopback playback stream would auto-connect straight back into it,
> creating a feedback loop. On a normal laptop the real device wins (our sink
> has priority −1), so plain `--loopback` is fine.

### 6.3 Two-machine operation (needs A + B)

```bash
# 🎧 B — start first; registers playback and waits for RFCOMM
./src/aether_receiver --verbose

# 🖥️ A
./src/aether_sender --target BB:BB:BB:BB:BB:BB --mode nl --verbose

# 🖥️ A — then play anything into the AetherCodec sink
pw-play --target aether_codec_sink your_music.flac
```

**Expect:** audio on B's headphones, and receiver stats showing
`recv= played= lost= buffer=…ms underruns=`. Sustained playback should hold
`lost=0` and `underruns=0`; rising underruns means the link cannot keep up
(check §3.2 throughput) — that is what Phase 5's ABR will address.

### 6.4 Known environment limitation

PipeWire needs **something to clock the graph**. On a normal laptop the audio
hardware does it. On a headless/SSH box with no claimed sound card, PipeWire's
`Dummy-Driver` fills in — check it exists:

```bash
pw-cli ls Node | grep -i driver     # expect Dummy-Driver / Freewheel-Driver
```

If the sink reaches `streaming` but `frames` never advances and `pw-play` hangs,
the graph has no driver. The sink deliberately does **not** set
`node.driver=true`: a `pw_stream` supplies no timing, so claiming to be a driver
gets this node selected to clock the graph and then stalls it.

---

## 7. Phase 5 — Adaptive bitrate engine

The quality ladder (lower row = worse link):

| State | Trigger | Mode / rate | Target |
|---|---|---|---|
| `NL-96kHz` | RSSI > −65, loss < 1% | near-lossless 96 kHz | ~1400 kbps |
| `NL-48kHz` | RSSI > −75, loss < 3% | near-lossless 48 kHz | ~800 kbps |
| `HQ-96kHz` | RSSI > −80, loss < 8% | perceptual 96 kHz | ~1000 kbps |
| `HQ-48kHz` | worse than the above | perceptual 48 kHz | ~600 kbps |

> Two things worth knowing before you read a range test:
> - The bitrates are **not** monotonically decreasing (1400 → 800 → **1000** → 600).
>   The `NL-48k → HQ-96k` step *raises* demand on an already-degrading link. That
>   is what the PRD specifies — it is a quality-preference ladder (stay lossless
>   by dropping rate first, then accept lossy at full rate) — so it is
>   implemented as written.
> - `NL-48kHz` is bit-exact **with respect to the decimated signal**, but the
>   96→48 kHz conversion itself discards the top octave. It is a degraded
>   fallback, not a lossless path end to end.

### 7.1 State machine & hysteresis — `test_abr` (💻)

```bash
./tests/test_abr
```

**Expect** all seven checks, notably:
- `downgrade after 2 polls (1000 ms)` — meets the "within 1 second" checkpoint
- `upgrade held off until 3000 ms elapsed` — meets the "wait 3 seconds" checkpoint
- `alternating good/bad link causes no switching` — a flapping link must not oscillate
- `walking away steps NL-96k → NL-48k → HQ-96k → HQ-48k`

The test injects a clock, so the 3-second hold is verified deterministically
rather than by sleeping.

### 7.2 Resampler — `test_resample` (💻)

```bash
./tests/test_resample
```

**Expect:**
- `1 kHz round-trip amplitude ratio` ≈ **1.00** (currently 1.002) — audible band
  passes through the 96→48→96 round trip untouched
- `35 kHz attenuation through decimator` below **−40 dB** (currently −65.8 dB) —
  content above the 48 kHz Nyquist is rejected, not folded back as aliasing

### 7.3 Watch the ladder live — `--abr-demo` (💻)

RSSI needs a real Bluetooth link, so `--abr-demo` sweeps a *simulated* RSSI up
and down to exercise the real engine on one machine:

```bash
# terminal 1
./src/aether_sender --loopback --no-play --mode auto --abr-demo --verbose

# terminal 2 — keep audio flowing so frames are actually encoded
sox -n -r 96000 -b 24 -c 2 /tmp/long.wav synth 30 sine 440 vol 0.5
pw-play /tmp/long.wav
```

**Expect** transitions, and the encoder following them:

```
[abr] NL-96kHz -> NL-48kHz (RSSI=-68 dBm)
[abr] NL-48kHz -> HQ-48kHz (RSSI=-80 dBm)
[abr] HQ-48kHz -> NL-48kHz (RSSI=-71 dBm)
[stats] frames=600 ... mode=HQ rate=48000
[stats] frames=700 ... mode=NL rate=96000
```

The `mode=`/`rate=` fields changing is the proof the switch reached the encoder.
There should be **no** `error`/`fail` lines across the switches.

> A fast sweep can skip a rung (e.g. `NL-48k → HQ-48k` without stopping at
> `HQ-96k`). That is correct: after N consecutive degraded readings the engine
> commits to the *latest* classification, and a 3 dBm-per-poll sweep can cross
> two bands before committing. A real walk-away is far slower.

### 7.4 Real RSSI (needs A + B)

```bash
sudo setcap cap_net_raw+ep ./src/aether_sender    # once
./src/aether_sender --target BB:BB:BB:BB:BB:BB --mode auto --verbose
```

Then walk Laptop A away from B and watch the transitions; walk back and confirm
upgrades wait ~3 s. Cross-check the reading with `hcitool rssi BB:BB:...`.

Without `CAP_NET_RAW` the daemon prints a one-time warning and **holds** the
current quality rather than assuming the worst — an unreadable RSSI is not
evidence of a bad link.

> **Not implemented:** packet-loss feedback. The ladder's loss thresholds are
> honoured by `abr_classify`, but the receiver's `CTRL_STATS_REPLY` back-channel
> does not exist yet, so the live engine drives on RSSI alone (loss is passed as
> 0). Loss-driven switching is covered by `test_abr` but cannot yet happen on a
> real link.

---

## 8. Phase 6 — End-to-end demo & measurement

> **Note on scope.** The implementation plan's Phase 6 sketch assumed a live
> `aether_sender --input FILE --record-output FILE` mode; that daemon is built
> around PipeWire's capture ring buffer (Phase 4), not file I/O, so bolting a
> parallel file path onto it would just be a second, redundant code path.
> Instead Phase 6 adds `aether_encode`/`aether_decode` as **standalone file
> tools** — WAV straight through `AetherEncoder`/`AetherDecoder`, no PipeWire
> or Bluetooth involved — which is actually the faster path for repeatable
> single-machine SNR/bitrate measurement. See `docs/AetherCodec_IMPLEMENTATION.md`
> Phase 6 for the full rationale.

### 8.1 File-to-file encode/decode + SNR — `aether_encode` / `aether_decode` (💻)

```bash
cd build

# Near-Lossless: should be bit-exact
./tools/aether_encode  test_hires.wav  /tmp/nl.aether  --mode nl
./tools/aether_decode  /tmp/nl.aether  /tmp/decoded_nl.wav
python3 ../tools/compare_spectra.py test_hires.wav /tmp/decoded_nl.wav "AetherCodec NL"

# Perceptual HQ: lossy, judge by audible-band SNR
./tools/aether_encode  test_hires.wav  /tmp/hq.aether  --mode hq
./tools/aether_decode  /tmp/hq.aether  /tmp/decoded_hq.wav
python3 ../tools/compare_spectra.py test_hires.wav /tmp/decoded_hq.wav "AetherCodec HQ" --shift 512
```

Need a WAV file first — `sox input.flac test_hires.wav` if you're starting
from FLAC (the tool only reads WAV: 16/24/32-bit integer PCM, mono or
stereo). `compare_spectra.py` needs `numpy`; `matplotlib` is optional (its
absence just skips the spectrogram PNG).

**Expect:**
- NL: `aether_encode` reports a ratio and bitrate (~1.4x on a pure tone, ~2–3x
  on real music — same ballpark as §4's table); `compare_spectra.py` reports
  `SNR full-band` **> 120 dB** and `Lossless (>120dB full-band): YES`. You can
  also verify with a raw-PCM `cmp`:
  ```bash
  sox test_hires.wav       -t raw -e signed -b 24 -r 96000 -c 2 a.raw
  sox /tmp/decoded_nl.wav  -t raw -e signed -b 24 -r 96000 -c 2 b.raw
  cmp a.raw b.raw && echo "bit-identical"
  ```
- HQ: **always pass `--shift 512`** (`MDCT_HOP`) — without it the decoded
  signal is misaligned by one hop and the SNR is meaningless (measured: −5 dB
  misaligned vs. **28.4 dB** aligned on the same file, matching
  `test_codec_hq`'s ~27.5 dB reference). `Lossless: NO` is correct for HQ —
  that's by design, judge it on the audible-band SNR number instead.
- A spectrogram PNG (`spectra_comparison.png`) if `matplotlib` is installed:
  three panels (original / decoded / difference).

> **Divergence from the doc's sketch:** the original Step 6.1 script computed
> only a naive full-band SNR. Per this repo's own convention (CLAUDE.md, and
> `test_codec_hq`'s "two SNRs" comment), full-band SNR is the wrong number for
> HQ mode at 96 kHz — the psychoacoustic model correctly zeroes everything
> above the ~17.6 kHz absolute threshold of hearing, and full-band SNR
> punishes it for that. `compare_spectra.py` reports both numbers; use
> audible-band for HQ.

### 8.2 Live two-machine demo (needs A + B)

Everything here already exists from Phase 4/5 — no new flags needed:

```bash
# 🎧 B — start first
./src/aether_receiver --verbose

# 🖥️ A — near-lossless
./src/aether_sender --target BB:BB:BB:BB:BB:BB --mode nl --verbose
pw-play --target aether_codec_sink test_hires.flac

# 🖥️ A — perceptual HQ (Ctrl+C the sender above first)
./src/aether_sender --target BB:BB:BB:BB:BB:BB --mode hq --verbose
pw-play --target aether_codec_sink test_hires.flac

# 🖥️ A — adaptive bitrate while walking away
./src/aether_sender --target BB:BB:BB:BB:BB:BB --mode auto --verbose
pw-play --target aether_codec_sink test_hires.flac
```

**Expect:** the sender's `[stats] frames= kbps= mode= rate=` line and the
receiver's `recv= played= lost= buffer=…ms underruns=` line — the same
bitrate/loss logging §6.3 and §7 already exercise. Nothing new to look for
here; Phase 6 doesn't add live-link tooling beyond what Phase 4/5 already has.

### 8.3 What needs real hardware (not automatable here)

| Measurement | Why it's out of scope for tooling |
|---|---|
| Latency (click-track round trip) | Needs an acoustic or loopback-cable capture on B to measure delay in samples; no such capture path exists on a single dev machine |
| THD+N | Needs an audio analyzer (e.g. REW) listening to real decoded output |
| MUSHRA listening score | Needs human listeners doing an A/B/X comparison |
| LDAC comparison | Needs a real A2DP/LDAC-capable device — this project deliberately doesn't implement A2DP |

Record these manually in the comparison table
(`docs/AetherCodec_IMPLEMENTATION.md` §6.4) once measured.

---

## 9. Common issues

| Symptom | Likely cause | Fix |
|---|---|---|
| `connect: Connection refused` | B not listening | Start the `server`/receiver side **first** |
| `bind: Address already in use` | old process holding channel | `pkill rfcomm_ping` (or the stale tool) |
| `cmake` fails at `pkg_check_modules` | dev packages missing | Install deps (Implementation Plan Step 0.1) |
| Tests "pass" but assert nothing | built with `NDEBUG` | Tests force `-UNDEBUG`; rebuild clean if edited |
| No audio in 3.3 | wrong sink / rate | Check `aplay -l`; keep rate/format flags matched |
| `could not register PipeWire sink` | no PipeWire session | `systemctl --user start pipewire`; check `pgrep pipewire` |
| Sink shows up but `frames` stays 0 | graph has no driver | See §6.4 — need hardware or `Dummy-Driver` |
| `pw-play --target NAME` prints help | 0.3.48 wants a numeric id | Use the id from `pw-play --list-targets` |
| Loopback howls / feeds back | our sink is the default output | Add `--no-play` (see §6.2) |
| `cannot read RSSI` warning | missing CAP_NET_RAW or no link | `sudo setcap cap_net_raw+ep ./src/aether_sender` |
| ABR never switches | RSSI unreadable → quality held | See §7.4; or use `--abr-demo` to test the engine |
| ABR skips a rung | sweep crossed two bands in one commit | Expected — see note in §7.3 |
| HQ `compare_spectra.py` SNR reads ~−5 dB | forgot `--shift 512` | See §8.1 — HQ has a one-hop OLA decode delay |
| `aether_decode` errors on a plain WAV/RFCOMM dump | not an `.aether` file | It needs the 8-byte `aether_encode` file header, not a raw packet dump |

---

## 10. Quick checklist

- [ ] `make` — clean build, no warnings
- [ ] `ctest` — 9/9 pass
- [ ] `rfcomm_ping` — 10 pings received on B
- [ ] `rfcomm_bench` — throughput recorded: ______ kbps
- [ ] `raw_stream` — audible on B
- [ ] `test_lpc` / `test_codec_nl` — 0 mismatches (bit-perfect)
- [ ] `test_mdct` — reconstruction error < 1e-5
- [ ] `test_codec_hq` — audible SNR ~27 dB, broadband bitrate in 900–1,100 kbps
- [ ] `test_ring` / `test_jitter` — pass
- [ ] Sink appears as "AetherCodec Hi-Res BT" in `pw-play --list-targets`
- [ ] `--loopback` — `frames` counter advances while playing audio in
- [ ] Two-machine: audio on B's headphones, `lost=0 underruns=0`
- [ ] `test_abr` — downgrade ≤ 1 s, upgrade held 3 s, no oscillation
- [ ] `test_resample` — 1 kHz ratio ≈ 1.00, 35 kHz rejected < −40 dB
- [ ] `--abr-demo` — transitions logged and `mode=`/`rate=` follow, no errors
- [ ] Real range test: NL-96k held at 0–1 m, degrades gracefully at 5 m+
- [ ] `aether_encode`/`aether_decode` NL round-trip — bit-identical raw PCM
- [ ] `aether_encode`/`aether_decode` HQ round-trip — `compare_spectra.py --shift 512` audible SNR ~27 dB
- [ ] Real range/listening tests (§8.3): latency, THD+N, MUSHRA, LDAC comparison — pending real hardware
