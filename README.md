# AetherCodec

Open-source, Linux-native, **lossless-first** Bluetooth audio codec written in C.
Streams 24-bit/96kHz stereo between two laptops over raw **RFCOMM** (not A2DP),
with two runtime-selectable modes:

- **Near-Lossless (NL)** — LPC + Rice coding, bit-perfect round-trip (~3x, ~1,400 kbps)
- **Perceptual HQ** — MDCT + psychoacoustic masking, transparent (~1,000 kbps)

An adaptive-bitrate engine monitors RSSI + packet loss and degrades gracefully
before it ever drops audio.

> Full design in [AetherCodec_HLD.md](docs/AetherCodec_HLD.md); step-by-step build in
> [AetherCodec_IMPLEMENTATION.md](docs/AetherCodec_IMPLEMENTATION.md); requirements in
> `AetherCodec_PRD.docx`.

## Status

Project scaffolding (Phase 0). Directory layout and public headers are in place;
`.c` implementations land phase by phase per the implementation plan.

## Layout

```
include/   public headers (packet, transport, codecs, encoder/decoder, jitter, abr)
src/       encoder/ decoder/ transport/ abr/ jitter/ os/ daemon/
tests/     unit tests (test_packet, test_lpc, test_mdct, test_jitter)
tools/     rfcomm ping/bench, raw streamers, analysis scripts
```

## Dependencies (Linux)

`build-essential cmake pkg-config libbluetooth-dev bluez libpipewire-0.3-dev
libasound2-dev libfftw3-dev sox`

## Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DAETHER_ENABLE_PIPEWIRE=ON \
         -DAETHER_ENABLE_ALSA=ON
make -j$(nproc)
make test
```

## Run

```bash
# Laptop B (receiver) — start first
aether_receiver

# Laptop A (sender) — target Laptop B's Bluetooth address
aether_sender --target AA:BB:CC:DD:EE:FF --mode nl
```

## License

Open source (MIT/Apache-2.0 intended). Zero paid dependencies.
