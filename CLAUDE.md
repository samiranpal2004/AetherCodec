# CLAUDE.md

Guidance for working in the AetherCodec repo.

## What this is

A custom **Bluetooth audio codec in C (C11)** streaming 24-bit/96kHz stereo
between two Linux laptops over **raw RFCOMM sockets** (BlueZ) — deliberately
**not** A2DP. Two codec modes: **Near-Lossless (NL)** = LPC + Rice (bit-perfect),
**Perceptual HQ** = MDCT + psychoacoustics (lossy). Plus an adaptive-bitrate
engine and PipeWire integration. Zero paid dependencies.

Full spec lives in `docs/`: `AetherCodec_PRD.docx`, `AetherCodec_HLD.md`
(design reference), `AetherCodec_IMPLEMENTATION.md` (the step-by-step build plan
we follow), `MANUAL_TESTING.md`.

## Build & test

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DAETHER_ENABLE_PIPEWIRE=ON -DAETHER_ENABLE_ALSA=OFF
make -j$(nproc)
ctest --output-on-failure
```

- PipeWire ON builds `aether_sender`/`aether_receiver`; OFF still builds
  everything else. ALSA stays OFF (fallback plugin not implemented).
- `aether_sender --loopback [--no-play]` exercises capture→encode→decode→playback
  on one machine — the only way to test Phase 4 without the second laptop.
- Bash tools here may be sandboxed and see missing deps; pass
  `dangerouslyDisableSandbox: true` to probe the real toolchain / run the build.
- Tests compile with `-UNDEBUG` so `assert()` stays live in Release. When adding
  an assert-based test, keep that flag on the target or it checks nothing.

## Non-negotiable invariant

**NL mode must be bit-perfect.** Encoder and decoder run *identical integer
arithmetic* over identical history (Q15 predictor, arithmetic `>>`); any change
that makes decode diverge from the input is a bug, not a tradeoff. The
`test_lpc` / `test_codec_nl` "0 mismatches" checks guard this — never relax them.

## Architecture

```
include/            public headers / interface contracts (one per module)
src/transport/      aether_packet.c (pack/unpack + CRC16/32), transport_rfcomm.c
src/encoder/        codec_lpc_enc.c, aether_encoder.c   (+ codec_mdct_enc.c later)
src/decoder/        codec_lpc_dec.c, aether_decoder.c   (+ codec_mdct_dec.c later)
src/os/            audio_ring.c (SPSC), pw_sink.c (virtual sink), pw_playback.c
src/jitter/        jitter_buf.c (reorder + loss detect)
src/daemon/        aether_sender.c, aether_receiver.c
src/abr/           abr_ctrl.c (ladder+hysteresis), bt_rssi.c (HCI), resample.c (2:1)
tests/  tools/      unit tests; RFCOMM/stream diagnostics
```

Two static libs today: `aether_transport` (packet+RFCOMM, links `bluetooth`) and
`aether_codec` (LPC+Rice+encoder/decoder, links `aether_transport` + `m`). New
sources get added to the matching `src/CMakeLists.txt` target as each phase lands.

**NL payload format** (self-describing, not fixed-size): `[frame_samples:u16][flags:u8]`
(flags bit0 = mid/side: `m=(l+r)>>1, s=l−r`, parity of `s` restores the lost bit)
then per coded channel `[order:u8][rice_k:u8][wasted:u8][coeffs:order·i32][warmup:order·i32][rice_len:u16][rice_bytes]`
— `wasted` = shared low zero bits shifted out pre-LPC, restored post-synthesis.

**HQ payload format**: `[frame_samples:u16]` (total; 1–4 hops, multiple of
`MDCT_HOP` — daemons batch `AETHER_HQ_HOPS_PER_PKT=4` to amortise the 24-byte
header) then per hop `[flags:u8]` (bit0 = mid/side, applied to MDCT
*coefficients* so the decoder's OLA history stays in L/R space) then per coded
channel `[band_mask:u64][sf_rice_k:u8][sf_len:u16][sf_bytes][cf_rice_k:u8][cf_len:u16][cf_bytes]`
— scalefactors and coefficients cover only the bands set in the mask.

**Transport**: RFCOMM default; `--l2cap` on both daemons + `rfcomm_bench`
selects L2CAP `SOCK_SEQPACKET` (PSM 0x1001, one packet per SDU) — experimental,
benchmark per hardware pair. **Receiver → sender back-channel**: the receiver
sends `CTRL_STATS_REPLY` (`AetherStatsReply`: loss, buffer, underruns) every
~500 ms; the sender's reader thread must drain the socket in *every* mode (an
unread reverse path eventually blocks the receiver), and ABR gets real loss for
its 1/3/8% thresholds (stale > 2 s ⇒ treated as 0).

## Intentional divergences from the spec docs

The provided docs contain a few inconsistencies we corrected in code. Keep these —
don't "fix" them back to match the prose:

- `AETHER_MAGIC = 0xAE7EC0DE` (prose writes `0xAE7HC0DE`, not valid hex).
- `AETHER_HEADER_SIZE = 20`, not 24 (matches the packed struct + packet diagram;
  guarded by a `_Static_assert`).
- `LPC_RICE_MAX_PARAM = 30`, not 15 (15 overflows the unary code on noise and
  breaks losslessness; `k` is a wire byte, so widening is free).
- FFTW uses the single-precision module `fftw3f` (`fftwf_*`).
- **MDCT = fold + DCT-IV (`FFTW_REDFT11`)**, not the doc's r2c-plan sketch (which
  called non-existent `fftwf_creal`/`fftwf_cimag` and wasn't a valid MDCT).
- `bessel_i0` uses `term *= (x/2)/k; sum += term*term` (the doc squares `term`
  in place, which is wrong — it admits so inline).
- Spreading function: −2.5 dB upward / **−6.0 dB downward** (the doc's 1.5 dB
  downward makes that skirt shallower, which is backwards).
- Bark bands normalise by the Bark value **at Nyquist**, not a fixed 24 (at 96kHz
  a fixed 24 dumps ~2/3 of the spectrum into the last band); ATH dB is clamped
  before `powf` (the raw formula hits ~5300 dB at 48kHz and overflows).
- SMR defaults to **30 dB**, not the doc's 12 — the rate/quality knob (~6 dB per
  bit per significant coefficient). It is a *runtime* value (`mdct_set_smr_db`),
  not a constant: a fixed quality means an uncontrolled bitrate, and the sender
  closes an AIMD loop on it (`abr_smr_step`) so HQ fits whatever the link
  actually carries. Dropped frames are far more audible than coarse quantisation.
  The controller's ceiling is **54 dB** (codec clamp 60) — it was 30, which
  pinned HQ at ~480 kbps on a measured ~900 kbps link; with the higher ceiling
  the queue, not the constant, stops the climb, so spare link capacity becomes
  quality. The increase is two-tier (+1.2 dB/tick while the queue is idle,
  +0.4 near capacity) so recovery from a congestion cut doesn't take ~26 s.
- HQ entropy stage is **Rice, not Huffman** — the HLD's "fixed tables v1.0" are
  never specified anywhere, so untrained tables would be arbitrary.
- **HQ payload carries a 64-bit band mask** (not in the HLD): Rice never codes a
  symbol in zero bits, so the ~63% of 96 kHz bins that the ATH correctly zeroes
  still cost a bit each. That floored HQ-96k near 500 kbps whatever the quantiser
  did. Skipping dead bands costs 8 bytes/channel/frame and roughly halves the
  rate at identical SNR.

- **The virtual sink lives on the SENDER**, not the receiver (Step 4.1 says B,
  Step 4.4 contradicts it with A; only A works for the stated goal). Receiver is
  a plain playback client.
- **Never set `node.driver=true` on the virtual sink.** A `pw_stream` has no
  timing source; claiming driver gets it selected to clock the graph and stalls
  everything (reaches `streaming`, `process()` never fires, clients hang).
- `pw_buffer.requested` needs PipeWire ≥ 0.3.49 — guard with `PW_CHECK_VERSION`.
- PipeWire boundary uses `S32` with exact `>>8`/`<<8`; no float, so NL stays
  bit-exact end to end.
- **ABR ordering: lower enum == better quality**, so `target > current` means the
  link *degraded*. Step 5.2 has this backwards (it would upgrade on failure).
- ABR ladder bitrates are non-monotonic (1400→800→**1000**→600) — that's the
  PRD's quality-preference ladder, not a bug; don't "fix" it.
- Mode/rate switches need no control packet: every header carries `mode` +
  `sample_rate` and the decoder flushes on change (survives packet loss, unlike
  a one-shot `CTRL_CODEC_CHANGE`).
- **Sender decouples encode from send** (bounded packet queue + send thread):
  `rfcomm_send_packet` blocks, and blocking the encode loop on big NL frames
  drops captured audio and makes delivery bursty (stutter). Don't reintroduce a
  direct send from the encode loop. The queue is bounded by **queued audio time**
  (`SENDQ_MAX_MS`), never by slot count — a 2048-sample NL frame and a
  512-sample HQ frame are 4x apart in duration, so counting slots silently gives
  HQ a quarter of the slack.
- **Don't hand-chunk sends at `AETHER_RFCOMM_MTU`.** BlueZ's
  `rfcomm_sock_sendmsg` already fragments at the negotiated DLC MTU; pre-splitting
  only forces extra RFCOMM frames. One `send()` per packet, looping only on short
  writes.
- **ABR also reacts to send-queue backpressure**, not just RSSI (`abr_update_congested`):
  a strong link can still be unable to carry NL-96k. Congestion drops a rung
  immediately and holds a ceiling for `ABR_PROBE_INTERVAL_MS` to avoid oscillation.
- **`auto` on a real link starts at HQ-96k** (`abr_start_at`), not the HLD's
  "optimistic" NL-96k: measured NL-96k is ~3 Mbps on real music, which no RFCOMM
  link carries, so the optimistic start flooded the queue and cascaded to
  HQ-48k/SMR-12 in the first second of every session. The start rung is also the
  initial ceiling — NL must be earned via the headroom-gated probe. Loopback and
  `--abr-demo` keep the NL-96k start (no real link; the demo sweeps the ladder).
- **ABR switches flush the send queue** — the old mode's backlog is already
  ~500 ms late and, left to drain, reads as the *new* mode being congested too
  (that's the cascade above). Flushed packets are counted separately from
  overflow drops: a flush must never feed the congestion or SMR-cut signals, or
  each switch would trigger the next. The stats `dropped=` prints the sum so the
  receiver's `lost` still matches.
- **Receiver playback: fill one quantum, never `maxsize`.** PipeWire sizes
  `maxsize` for its max quantum; filling it over-drains the ring several-fold
  (huge `underruns` with `lost=0`). Pin the buffer via `SPA_PARAM_Buffers` +
  `NODE_LATENCY`; `pw_buffer.requested` is absent before 0.3.49.
- **Receiver prebuffer (`PLAY_PREBUFFER_MS`) must exceed the sender's queue hold
  (`SENDQ_MAX_MS`).** The sender delays a packet up to `SENDQ_MAX_MS` (500 ms)
  during a Bluetooth stall before dropping; if the receiver cushion is smaller,
  it underruns on every such stall *even with no packet lost* — `underruns`
  climbing in chunks while `lost=0 dropped=0` is the signature. Cushion is
  600 ms (> 500 ms) so a burst the sender rides out by queuing, the receiver
  rides out by buffering. Trades latency for smoothness — right for music.
- NL-48K is lossless only *relative to the decimated signal*; 96→48 itself is
  lossy. Don't claim end-to-end bit-exactness for the 48k states.

HQ mode is lossy: judge it by **audible-band** SNR (~27.5 dB) and bitrate, never
by bit-exactness. At 96kHz the ATH correctly zeroes everything above ~17.6 kHz,
so full-band SNR understates quality badly on broadband content.

**When you find another doc/code conflict:** pick the correct behavior, add a
short code comment explaining why, and note it under the relevant checkpoint in
`docs/AetherCodec_IMPLEMENTATION.md`. Also tell the user in your reply.

## Workflow conventions

- Follow the phases in `docs/AetherCodec_IMPLEMENTATION.md` in order. After
  finishing a phase, tick its checkpoint boxes there — check only what's actually
  verified, and annotate items blocked on the second laptop ("needs second paired
  laptop") rather than leaving them silently unchecked.
- Much of the system needs two paired laptops (RFCOMM ping/bench/stream, ABR
  RSSI). Development here is single-machine: prove as much as possible with unit
  tests, and clearly mark what can't be run yet.
- Don't commit or branch unless asked.
