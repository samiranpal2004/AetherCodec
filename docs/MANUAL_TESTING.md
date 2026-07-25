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
`lost=0` and `underruns` near 0 and flat.

> **Stutter?** The bitrate a mode needs may exceed what the link smoothly
> carries — NL-96k (~1,400 kbps, often higher on dense music) is the hardest.
> The sender decouples encode from send (a queue + send thread) and the receiver
> prebuffers ~250 ms, so a mode that *fits* plays cleanly. If a **fixed** mode
> still stutters, the link can't carry it — use `--mode auto`, which now steps
> down on **send-queue backpressure** (see §7), not just RSSI. Watch the
> sender's `queue=`/`dropped=` fields: a queue that stays high or a rising
> `dropped` count means that mode is over budget.

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

> **Backpressure (the important one for a strong but slow link).** RSSI can be
> excellent while the link still cannot carry NL-96k. The engine therefore also
> watches the sender's **send-queue depth**: when it backs up, `auto` drops a
> rung immediately (even at −16 dBm) and won't climb back into that mode for
> ~20 s, so it settles on the best mode the link can actually sustain. This is
> why `auto` can sound clean where fixed NL-96k stutters. Covered by `test_abr`
> (`congestion steps down despite strong RSSI`).
>
> **Not implemented:** on-air packet-loss feedback. `abr_classify` honours the
> loss thresholds (and `test_abr` covers them), but the receiver's
> `CTRL_STATS_REPLY` back-channel does not exist yet, so loss itself isn't
> reported back — congestion is inferred from the send queue instead.

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

## 9. Full two-laptop test run (A + B)

This is the consolidated bring-up procedure — everything that needs both
machines, in the order to do it. **Each step gates the next**: if transport
fails, nothing above it can work, so don't skip ahead. The single-machine tests
(§1–2, §5–7) should already pass on **both** laptops before you start here.

> Throughout: **A** = sender, **B** = receiver. Commands are labelled 🖥️ **A**
> or 🎧 **B**. You only ever need **B's** Bluetooth address — A connects to B,
> and A reads the RSSI of its link *to* B. B just listens.

### 9.0 Prerequisites (both laptops)

On **each** laptop:

```bash
# same checkout, clean build, all unit tests green
cd ~/aethercodec/build && ctest        # expect 9/9

# PipeWire session is up
pgrep -a pipewire                      # must show a running server

# note this machine's Bluetooth address
hciconfig hci0 | grep "BD Address"
```

Write down both addresses. Below, `BB:BB:BB:BB:BB:BB` is **B's** address.

> Both laptops must run the **same build** — the wire format has no version
> negotiation, so a stale binary on one side will mis-decode. Rebuild both from
> the same commit if in doubt.

### 9.1 Pair A and B (once)

Skip if `bluetoothctl info BB:BB:BB:BB:BB:BB` on A already shows
`Paired: yes` / `Trusted: yes`.

```bash
# 🎧 B — make discoverable
bluetoothctl
  power on
  agent on
  discoverable on
  pairable on

# 🖥️ A — pair, trust, connect
bluetoothctl
  power on
  agent on
  scan on            # wait for B to appear, then:
  pair  BB:BB:BB:BB:BB:BB
  trust BB:BB:BB:BB:BB:BB
  connect BB:BB:BB:BB:BB:BB
```

**Gate:** `bluetoothctl info BB:BB:BB:BB:BB:BB` on A shows
`Paired: yes, Trusted: yes, Connected: yes`. If pairing fails, nothing below
will work — fix it here.

### 9.2 Transport sanity — Phase 1 (do this first, always)

Prove the raw pipe before involving the codec. Details in §3.

```bash
# 🎧 B                         # 🖥️ A
./tools/rfcomm_ping server     ./tools/rfcomm_ping client BB:BB:BB:BB:BB:BB
```
**Gate:** B prints `seq=0 … seq=9`. `Connection refused` on A ⇒ B side not
started first, or channel busy (`pkill rfcomm_ping` on B).

```bash
# 🎧 B                          # 🖥️ A
./tools/rfcomm_bench server      ./tools/rfcomm_bench client BB:BB:BB:BB:BB:BB
```
**Record the throughput.** This is the ceiling everything else lives under:
- ≥ 1000 kbps → 24/96 NL is feasible.
- 600–1000 kbps → expect NL-48k / HQ under load.
- < 600 kbps → no EDR; NL-96k will underrun, use `--mode auto` or `hq`.

Measured throughput: **_1003_____ kbps**

Optional — raw PCM audio (§3.3) confirms B's DAC/headphones work end to end.

### 9.3 Codec streaming — Phase 4

Start **B first** (it registers playback, then waits on RFCOMM); then **A**
(it connects, then registers the sink); then play into the sink on A.

```bash
# 🎧 B — leave this running; watch its stats
./src/aether_receiver --verbose

# 🖥️ A — near-lossless
./src/aether_sender --target BB:BB:BB:BB:BB:BB --mode nl --verbose

# 🖥️ A — third terminal: play a hi-res file INTO our sink
#   --target is mandatory: without it the audio goes to A's own speakers, not B.
pw-play --target aether_codec_sink your_music.flac
```

**Expect:**
- Audio on **B's** headphones.
- 🖥️ A: `[stats] frames= … kbps= mode=NL rate=96000` climbing.
- 🎧 B: `[stats] recv= played= lost=0 buffer=…ms underruns=0`.

**Gate:** `lost=0` and `underruns=0` over ~30 s. Rising `underruns` ⇒ the link
can't sustain NL-96k (compare against 9.2's number) — that's expected on a slow
link and is what §9.4's ABR handles. Rising `lost` ⇒ packets dropping on the
air; the receiver conceals with silence.

Then repeat in **HQ** (Ctrl+C the sender first — fixed modes don't switch live):

```bash
# 🖥️ A
./src/aether_sender --target BB:BB:BB:BB:BB:BB --mode hq --verbose
pw-play --target aether_codec_sink your_music.flac
```
HQ should sound transparent and sit lower/steadier on bitrate than NL.

### 9.4 Adaptive bitrate range test — Phase 5

Grant the sender RSSI access once, then run in `auto`:

```bash
# 🖥️ A — once (HCI access needs CAP_NET_RAW)
sudo setcap cap_net_raw+ep ./src/aether_sender

# 🎧 B
./src/aether_receiver --verbose

# 🖥️ A
./src/aether_sender --target BB:BB:BB:BB:BB:BB --mode auto --verbose
pw-play --target aether_codec_sink your_music.flac
```

Now **physically walk A away from B** and back, watching both consoles:

- 🖥️ A logs `[abr] NL-96kHz -> NL-48kHz (RSSI=… dBm)` as you move away, stepping
  down the ladder; walking back **upgrades**, and each upgrade waits ~3 s.
- 🎧 B logs `[receiver] stream rate -> 48000 Hz, mode=…` following the switches,
  and audio stays continuous (no dropouts) across them.
- Cross-check the reading any time with `hcitool rssi BB:BB:BB:BB:BB:BB` on A.

**Expect:** NL-96k held at 0–1 m; graceful step-down by ~5 m+; audio never cuts
out, only changes quality. If A prints `cannot read RSSI`, the `setcap` didn't
take (or you rebuilt the binary — re-run it) and ABR holds its current state.

> The live engine drives on **RSSI only** — packet-loss feedback
> (`CTRL_STATS_REPLY`) isn't implemented yet, so a link that's strong but lossy
> won't downgrade on its own. See §7.4.

### 9.5 Sustained run + record results

Play a full track (≥ 60 s) in each mode and capture the steady-state numbers:

| Mode | Bitrate (A `[stats]`) | lost | underruns | Audible quality |
|---|---|---|---|---|
| NL-96k | | | | |
| HQ-96k | | | | |
| auto (range) | | | | |

**Pass:** 60 s continuous in good conditions with `lost=0 underruns=0`, no
audible glitches, and (auto) graceful mode changes while moving. Transfer these
into the comparison table in `docs/AetherCodec_IMPLEMENTATION.md` §6.4.

### 9.6 Two-laptop checklist

- [ ] 9.1 Paired + trusted + connected (`bluetoothctl info`)
- [ ] 9.2 `rfcomm_ping` — 10 pings on B
- [ ] 9.2 `rfcomm_bench` — throughput recorded: ______ kbps
- [ ] 9.3 NL streaming — audio on B, `lost=0 underruns=0` over 30 s
- [ ] 9.3 HQ streaming — transparent, steady bitrate
- [ ] 9.4 `auto` — `[abr]` steps down/up while walking; B follows the rate; no dropouts
- [ ] 9.5 60 s sustained per mode — results recorded in §6.4 table

---

## 10. Common issues

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
| A `connect` fails with `Host is down`/`Connection timed out` | A and B not paired, or B not connected | Redo §9.1; confirm `bluetoothctl info` shows `Connected: yes` |
| Streaming works but audio comes out of **A's** speakers | forgot `--target aether_codec_sink` on `pw-play` | Our sink has priority −1, so A's real output stays default; always target the sink |
| B decodes garbage / loud noise | mismatched builds on A and B | Rebuild both from the same commit (no wire-format versioning) |
| Two-machine `garbled after a while`, `underruns` climbing | link below the codec's bitrate | Check §9.2 throughput; use `--mode auto` or `hq` |
| Stutter with **`lost=0` but huge `underruns`** (≫ playtime) | playback over-drained the ring (fixed) | Rebuild — playback now pins one quantum + prebuffers ~100 ms. `underruns` far exceeding total samples played meant the buffer was pulled several× realtime |
| Rare click every few minutes, otherwise smooth | A/B sample-clock drift | Expected for now — no async resampler; the ~100 ms prebuffer re-primes on a full drain |

---

## 11. Quick checklist

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
