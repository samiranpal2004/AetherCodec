# AetherCodec — Manual Testing Guide

A short, step-by-step guide to build AetherCodec and manually verify each phase
that is currently implemented (**Phase 0–2**). Later phases (PipeWire, ABR) will
be added here as they land.

> **Legend**
> - 🖥️ **A** = Laptop A (sender)   🎧 **B** = Laptop B (receiver)
> - 💻 = runs on a single machine (no second laptop needed)

---

## 1. Build (💻)

```bash
cd ~/aethercodec
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DAETHER_ENABLE_PIPEWIRE=OFF \
         -DAETHER_ENABLE_ALSA=OFF
make -j$(nproc)
```

**Expect:** builds with **no warnings or errors**. PipeWire/ALSA are OFF because
the OS-integration layer (Phase 4) isn't implemented yet.

---

## 2. Unit tests (💻)

```bash
cd ~/aethercodec/build
ctest --output-on-failure
```

**Expect:** `100% tests passed, 0 tests failed out of 3`.

Run them individually to see the detail:

```bash
./tests/test_packet     # packet pack/unpack + CRC corruption detection
./tests/test_lpc        # LPC + Rice bit-perfect round-trip (sine / noise / silence)
./tests/test_codec_nl   # full encoder → packet → decoder stereo round-trip
```

**Expect:** every line reports `LOSSLESS (0 mismatches)`. Any mismatch is a
**hard failure** — the near-lossless mode must be bit-perfect.

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

## 4. Phase 2 — Near-Lossless codec (💻 for now)

The codec is fully validated on one machine by `test_lpc` and `test_codec_nl`
(section 2). To sanity-check compression on **real music** you currently have to
read the ratio printed by `test_codec_nl`; a dedicated file-to-file tool
(`aether_encode` / `aether_decode`) is not built yet.

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

## 5. Common issues

| Symptom | Likely cause | Fix |
|---|---|---|
| `connect: Connection refused` | B not listening | Start the `server`/receiver side **first** |
| `bind: Address already in use` | old process holding channel | `pkill rfcomm_ping` (or the stale tool) |
| `cmake` fails at `pkg_check_modules` | dev packages missing | Install deps (Implementation Plan Step 0.1) |
| Tests "pass" but assert nothing | built with `NDEBUG` | Tests force `-UNDEBUG`; rebuild clean if edited |
| No audio in 3.3 | wrong sink / rate | Check `aplay -l`; keep rate/format flags matched |

---

## 6. Quick checklist

- [ ] `make` — clean build, no warnings
- [ ] `ctest` — 3/3 pass
- [ ] `rfcomm_ping` — 10 pings received on B
- [ ] `rfcomm_bench` — throughput recorded: ______ kbps
- [ ] `raw_stream` — audible on B
- [ ] `test_lpc` / `test_codec_nl` — 0 mismatches (bit-perfect)
