# AetherCodec

Open-source, Linux-native, **lossless-first** audio codec written in C11.
Streams 24-bit/96kHz stereo between two machines wirelessly over raw **RFCOMM**
or **L2CAP** Bluetooth sockets (deliberately **not** A2DP — every mainstream
Bluetooth audio codec is lossy and bandwidth-capped) or over **TCP/Wi-Fi**,
with two runtime-selectable codec modes:

- **Near-Lossless (NL)** — LPC + Rice coding, bit-perfect round-trip (~1,400 kbps)
- **Perceptual HQ** — MDCT + psychoacoustic masking, transparent quality (~1,000 kbps)

An adaptive-bitrate (ABR) engine watches RSSI, packet loss, and send-queue
congestion, and steps the quality ladder down before audio ever drops — then
climbs back up as headroom returns.

> Full design in [AetherCodec_HLD.md](docs/AetherCodec_HLD.md); phase-by-phase
> build log in [AetherCodec_IMPLEMENTATION.md](docs/AetherCodec_IMPLEMENTATION.md)
> (all 10 phases complete); manual test procedures in
> [MANUAL_TESTING.md](docs/MANUAL_TESTING.md); requirements in `docs/AetherCodec_PRD.docx`;
> project write-up in [PROJECT_SUMMARY.md](docs/PROJECT_SUMMARY.md).

## Status

All implementation phases (0–10) are complete: transport layer, both codec
modes, PipeWire integration, the ABR engine, end-to-end demo tooling,
link-efficiency upgrades, TCP transport, and playback-stability fixes. Some
checkpoint items remain marked "needs second paired laptop" where two-machine
hardware is required to verify (see the implementation doc).

## Why not A2DP?

Every standard Bluetooth audio codec (SBC, AAC, aptX, LDAC) throws away
information to hit its target bitrate, and none guarantees bit-exact
round-trips. A 24-bit/96kHz stereo lossless stream needs ~3+ Mbps — an order
of magnitude more than A2DP or even RFCOMM can reliably carry (~200 kbps
measured). AetherCodec sidesteps the profile entirely: it frames and streams
raw PCM itself, and adds a TCP/Wi-Fi transport as the path that actually
carries hi-res audio, while Bluetooth stays viable for the lower-bitrate
perceptual mode.

## Layout

```
include/           public headers (packet, transport, codecs, encoder/decoder, jitter, abr)
src/
  transport/       aether_packet.c (pack/unpack + CRC16/32), transport_rfcomm.c, transport_tcp.c
  encoder/         codec_lpc_enc.c, codec_mdct_enc.c, aether_encoder.c
  decoder/         codec_lpc_dec.c, codec_mdct_dec.c, aether_decoder.c
  os/              audio_ring.c (SPSC ring), pw_sink.c, pw_playback.c
  jitter/          jitter_buf.c (reorder + loss detection)
  daemon/          aether_sender.c, aether_receiver.c
  abr/             abr_ctrl.c (ladder + hysteresis), bt_rssi.c (HCI), resample.c (2:1)
tests/             unit tests (packet, lpc, mdct, jitter, ring, resample, abr, abr_switch, codec_nl, codec_hq)
tools/             rfcomm ping/bench, raw stream sender/receiver, file encode/decode, SNR + spectra analysis
docs/              HLD, implementation plan, manual testing guide, project summary
```

Two static libs today: `aether_transport` (packet framing + RFCOMM/TCP, links
`bluetooth`) and `aether_codec` (LPC+Rice+MDCT encoder/decoder, links
`aether_transport` + `m` + `fftw3f`), plus `aether_runtime` (ring buffer,
jitter buffer, ABR, resampler) used by the sender/receiver daemons.

## Dependencies (Linux)

```
build-essential cmake pkg-config libbluetooth-dev bluez libpipewire-0.3-dev libfftw3-dev sox
```

ALSA fallback is toggleable via `-DAETHER_ENABLE_ALSA` but not yet implemented
— leave it `OFF`.

## Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DAETHER_ENABLE_PIPEWIRE=ON \
         -DAETHER_ENABLE_ALSA=OFF
make -j$(nproc)
ctest --output-on-failure
```

`AETHER_ENABLE_PIPEWIRE=ON` builds `aether_sender`/`aether_receiver`; `OFF`
still builds the codec/transport libs, tests, and tools.

## Run

```bash
# Machine B (receiver) — start first
aether_receiver

# Machine A (sender) — Bluetooth, targeting B's address
aether_sender --target AA:BB:CC:DD:EE:FF --mode nl

# Or over Wi-Fi instead of Bluetooth (recommended for NL/HQ at full quality)
aether_receiver --port 7331
aether_sender --tcp --target 192.168.1.42:7331 --mode auto
```

`--mode` accepts `nl`, `hq`, or `auto` (ABR-driven). `--l2cap` selects the
experimental L2CAP `SOCK_SEQPACKET` transport instead of RFCOMM (mutually
exclusive with `--tcp`).

### Single-machine testing (no second laptop needed)

```bash
aether_sender --loopback [--no-play]
```

Exercises the full capture → encode → decode → playback pipeline locally —
the only way to validate most of the pipeline without a second machine.

### File-based encode/decode

```bash
aether_encode input.wav output.aether --mode nl
aether_decode output.aether restored.wav
```

## Testing

```bash
cd build && ctest --output-on-failure
```

Unit tests cover packet framing, bit-exact LPC/Rice round-trips (`test_lpc`,
`test_codec_nl` — must report 0 mismatches, this is the non-negotiable
lossless invariant), MDCT correctness, the jitter buffer, the SPSC ring
buffer, the resampler, and ABR state transitions/mode switching. See
[MANUAL_TESTING.md](docs/MANUAL_TESTING.md) for two-laptop and benchmark
procedures, and `tools/benchmark.sh` / `tools/aether_snr.py` /
`tools/compare_spectra.py` for bitrate/SNR/spectral analysis.

## License

Open source (MIT/Apache-2.0 intended). Zero paid dependencies.
