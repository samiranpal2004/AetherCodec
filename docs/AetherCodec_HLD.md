# AetherCodec — High Level Design (HLD)

> **Version:** 1.0 | **Date:** July 2026 | **Status:** Draft  
> **Language:** C | **Platform:** Linux (PipeWire/ALSA) + Windows (WASAPI)  
> **Authors:** AetherCodec Project Team

---

## Table of Contents

1. [Overview](#1-overview)
2. [System Architecture](#2-system-architecture)
3. [Component Breakdown](#3-component-breakdown)
4. [Transport Layer Design](#4-transport-layer-design)
5. [Codec Engine Design](#5-codec-engine-design)
   - 5.1 [Near-Lossless Mode — LPC + Rice](#51-near-lossless-mode--lpc--rice)
   - 5.2 [Perceptual HQ Mode — MDCT + Psychoacoustics](#52-perceptual-hq-mode--mdct--psychoacoustics)
6. [Adaptive Bitrate Engine](#6-adaptive-bitrate-engine)
7. [OS Integration Layer](#7-os-integration-layer)
8. [Packet Format Specification](#8-packet-format-specification)
9. [Data Flow Diagrams](#9-data-flow-diagrams)
10. [Module Interface Contracts](#10-module-interface-contracts)
11. [Memory & Threading Model](#11-memory--threading-model)
12. [Error Handling Strategy](#12-error-handling-strategy)
13. [Build System & Directory Layout](#13-build-system--directory-layout)
14. [Key Design Decisions & Rationale](#14-key-design-decisions--rationale)
15. [Limitations & Known Constraints](#15-limitations--known-constraints)

---

## 1. Overview

AetherCodec is a **custom Bluetooth audio streaming codec** implemented in C, designed to stream **24-bit / 96kHz stereo audio** between two Linux laptops with near-lossless or high-quality perceptual fidelity — surpassing LDAC, aptX HD, and SSC in both openness and audio accuracy.

### 1.1 Core Design Principles

| Principle | Description |
|---|---|
| **Lossless-first** | Degrade to perceptual encoding only when bandwidth forces it, never by default |
| **Transport-agnostic codec** | Codec layer is decoupled from Bluetooth transport — can run over any byte stream |
| **No A2DP dependency** | Uses raw RFCOMM sockets, bypassing the standard Bluetooth audio stack entirely |
| **Dual-mode** | Near-lossless (LPC/Rice) and perceptual HQ (MDCT/psychoacoustic) selectable at runtime |
| **OS-native integration** | Appears as a standard audio device via PipeWire on Linux |
| **Zero-cost dependencies** | All libraries are free and open-source |

### 1.2 What AetherCodec Is NOT

- It is **not** an A2DP codec profile (no SBC/AAC/aptX compatibility)
- It is **not** lossless at 24-bit/192kHz (BT bandwidth ceiling prevents this)
- It does **not** work with off-the-shelf Bluetooth headsets yet (requires firmware support — Phase 3)
- It is **not** a real-time conferencing codec (latency target is 40–80ms, not <20ms)

---

## 2. System Architecture

### 2.1 High-Level Block Diagram

```
╔══════════════════════════════════════╗     ╔══════════════════════════════════════╗
║           LAPTOP A (SENDER)          ║     ║          LAPTOP B (RECEIVER)         ║
║                                      ║     ║                                      ║
║  ┌────────────────────────────────┐  ║     ║  ┌────────────────────────────────┐  ║
║  │     Audio Source               │  ║     ║  │     PipeWire Virtual Sink      │  ║
║  │  (PipeWire capture / file)     │  ║     ║  │  "AetherCodec Hi-Res BT"       │  ║
║  └──────────────┬─────────────────┘  ║     ║  └──────────────┬─────────────────┘  ║
║                 │ PCM 24-bit/96kHz   ║     ║                 │ PCM 24-bit/96kHz   ║
║  ┌──────────────▼─────────────────┐  ║     ║  ┌──────────────▼─────────────────┐  ║
║  │       AetherCodec Encoder      │  ║     ║  │       AetherCodec Decoder      │  ║
║  │  ┌──────────┐ ┌─────────────┐  │  ║     ║  │  ┌──────────┐ ┌─────────────┐  │  ║
║  │  │ NL Mode  │ │  HQ Mode    │  │  ║     ║  │  │ NL Mode  │ │  HQ Mode    │  │  ║
║  │  │(LPC+Rice)│ │(MDCT+Psych) │  │  ║     ║  │  │(LPC+Rice)│ │(MDCT+Psych) │  │  ║
║  │  └──────────┘ └─────────────┘  │  ║     ║  └──────────┘ └─────────────┘  │  ║
║  │  ┌─────────────────────────┐   │  ║     ║  │  ┌─────────────────────────┐  │  ║
║  │  │  Adaptive Bitrate Ctrl  │   │  ║     ║  │  │  Jitter Buffer (40ms)   │  │  ║
║  │  └─────────────────────────┘   │  ║     ║  │  └─────────────────────────┘  │  ║
║  └──────────────┬─────────────────┘  ║     ║  └──────────────▲─────────────────┘  ║
║                 │ AetherPacket       ║     ║                 │ AetherPacket        ║
║  ┌──────────────▼─────────────────┐  ║     ║  ┌──────────────┴─────────────────┐  ║
║  │     Transport Layer            │  ║     ║  │     Transport Layer            │  ║
║  │  RFCOMM Socket (BlueZ)         │  ║     ║  │  RFCOMM Socket (BlueZ)         │  ║
║  └──────────────┬─────────────────┘  ║     ║  └──────────────▲─────────────────┘  ║
╚═════════════════╪════════════════════╝     ╚═════════════════╪════════════════════╝
                  │                                             │
                  └──────────── Bluetooth EDR ─────────────────┘
                                (~1,000–1,500 kbps)
                                                              │
                                                ┌─────────────▼──────────┐
                                                │   3.5mm Headphone Jack  │
                                                │   (Laptop B's DAC)      │
                                                └─────────────────────────┘
```

### 2.2 Process Model

```
Laptop A                                    Laptop B
────────                                    ────────
aether_sender (daemon)                      aether_receiver (daemon)
    │                                            │
    ├── capture_thread      (reads PipeWire)     ├── recv_thread       (reads RFCOMM socket)
    ├── encode_thread        (LPC or MDCT)       ├── decode_thread      (LPC or MDCT)
    ├── send_thread          (RFCOMM write)       ├── playback_thread    (writes PipeWire sink)
    └── bitrate_ctrl_thread  (monitors RSSI)     └── stats_thread       (reports quality)
```

---

## 3. Component Breakdown

### 3.1 Component Registry

| Component | Module Name | Language | Responsibility |
|---|---|---|---|
| Encoder Core | `aether_encoder` | C | Frame segmentation, mode dispatch, compression |
| Decoder Core | `aether_decoder` | C | Packet reassembly, decompression, PCM output |
| LPC Engine | `codec_lpc` | C | Linear prediction, Levinson-Durbin, Rice coding |
| MDCT Engine | `codec_mdct` | C | MDCT transform, Bark-scale mapping, psychoacoustic masking |
| Transport | `transport_rfcomm` | C | RFCOMM socket lifecycle, send/recv, fragmentation |
| Packet Layer | `aether_packet` | C | Pack/unpack AetherPacket, CRC check |
| Jitter Buffer | `jitter_buf` | C | Reorder, timestamp-driven playout, concealment |
| Adaptive Bitrate | `abr_ctrl` | C | RSSI + loss monitoring, mode switching signals |
| PipeWire Sink | `pw_sink` | C | Virtual audio device registration, sample delivery |
| PipeWire Source | `pw_source` | C | System audio capture for encoder input |
| ALSA Plugin | `alsa_aether` | C | Fallback for non-PipeWire systems |
| CLI Tool | `aetherd` | C | Daemon launcher, mode control, status display |

### 3.2 Dependency Graph

```
aetherd (CLI/daemon)
    ├── aether_encoder
    │       ├── codec_lpc
    │       ├── codec_mdct
    │       ├── abr_ctrl
    │       └── aether_packet
    ├── aether_decoder
    │       ├── codec_lpc
    │       ├── codec_mdct
    │       ├── jitter_buf
    │       └── aether_packet
    ├── transport_rfcomm
    │       └── aether_packet
    ├── pw_sink          (Laptop B)
    ├── pw_source        (Laptop A)
    └── alsa_aether      (optional fallback)
```

---

## 4. Transport Layer Design

### 4.1 Why RFCOMM, Not A2DP

Standard Bluetooth audio uses the **A2DP profile**, which mandates codec negotiation from a fixed list (SBC, AAC, aptX, LDAC). There is no mechanism to inject a custom codec into A2DP without a kernel driver. Instead, AetherCodec uses **RFCOMM** — a serial port emulation protocol over Bluetooth Classic — which gives a raw byte stream with no restrictions on payload format.

```
Standard A2DP Stack:
  App → PulseAudio/PipeWire → A2DP → [SBC/LDAC only] → BT chip → headset

AetherCodec Stack:
  App → PipeWire Source → AetherEncoder → RFCOMM socket → BT chip → RFCOMM socket → AetherDecoder → PipeWire Sink → DAC
```

### 4.2 RFCOMM Socket Lifecycle

```
SENDER (Laptop A)                           RECEIVER (Laptop B)
─────────────────                           ─────────────────────
socket(AF_BLUETOOTH, SOCK_STREAM,           socket(AF_BLUETOOTH, SOCK_STREAM,
       BTPROTO_RFCOMM)                             BTPROTO_RFCOMM)
                                            bind(addr, channel=1)
                                            listen(sock, 1)
connect(addr, channel=1)  ─────────────►   accept()
                           ◄─────────────   [connection established]
send(AetherPacket) ────────────────────►    recv(AetherPacket)
...streaming...
send(TEARDOWN packet) ─────────────────►    recv → close
close()                                     close()
```

### 4.3 Fragmentation

RFCOMM MTU is typically 672 bytes. Large audio frames must be fragmented:

```
AetherPacket (up to 4KB payload)
    │
    ├── Fragment 0: [FRAG_HDR: seq=42, frag=0/3] [data 0..671]
    ├── Fragment 1: [FRAG_HDR: seq=42, frag=1/3] [data 672..1343]
    └── Fragment 2: [FRAG_HDR: seq=42, frag=2/3] [data 1344..end]
```

Fragment header (4 bytes prepended to each chunk):

```c
typedef struct {
    uint16_t packet_seq;   // parent packet sequence number
    uint8_t  frag_index;   // this fragment's index (0-based)
    uint8_t  frag_total;   // total fragments for this packet
} FragHeader;
```

### 4.4 Bandwidth Budget

```
Target: 24-bit / 96kHz / Stereo

Raw PCM:              9,216 kbps  (way above BT limit)
NL mode (LPC ~3x):   ~1,400 kbps (tight, feasible on EDR)
HQ mode (MDCT ~5x):  ~1,000 kbps (comfortable on EDR)
BT EDR practical:     1,000–1,500 kbps

Safety margin (headers + retransmit overhead): ~80 kbps reserved
```

---

## 5. Codec Engine Design

### 5.1 Near-Lossless Mode — LPC + Rice

#### Overview

Linear Predictive Coding models each audio sample as a **linear combination of previous samples**. The prediction is subtracted from the actual sample to produce a **residual**. Residuals are statistically much smaller than raw samples and compress far better. Rice coding then entropy-encodes the residuals with zero information loss.

#### Algorithm Pipeline

```
Input PCM Frame (2048 samples × 24-bit)
         │
         ▼
┌─────────────────────────────┐
│  1. Windowing (rectangular) │  — no window needed for lossless
└──────────────┬──────────────┘
               │
               ▼
┌─────────────────────────────┐
│  2. Autocorrelation         │  — lag 0 to LPC_ORDER (8–32)
│     R[k] = Σ x[i]·x[i-k]   │
└──────────────┬──────────────┘
               │
               ▼
┌─────────────────────────────┐
│  3. Levinson-Durbin         │  — solve Yule-Walker equations
│     → LPC coefficients a[k] │    O(order²) per frame
└──────────────┬──────────────┘
               │
               ▼
┌─────────────────────────────┐
│  4. Residual Computation    │  — e[n] = x[n] - Σ a[k]·x[n-k]
│     (prediction error)      │    residuals are near-zero
└──────────────┬──────────────┘
               │
               ▼
┌─────────────────────────────┐
│  5. Rice Entropy Coding     │  — choose Rice param k per subframe
│     golomb_rice(e[n], k)    │    adaptive k minimizes bit count
└──────────────┬──────────────┘
               │
               ▼
         Compressed bitstream
         (LPC coeffs + Rice-coded residuals)
```

#### Key Parameters

| Parameter | Value | Notes |
|---|---|---|
| Frame size | 2048 samples | ~21ms at 96kHz |
| Subframe size | 512 samples | Rice param chosen per subframe |
| LPC order | 8–32 (adaptive) | Higher order = better prediction, more coefficients |
| Coefficient precision | 15-bit fixed point | Quantized with shift factor stored in frame header |
| Rice parameter range | 0–15 | Selected to minimize frame size |
| Warm-up samples | LPC_ORDER samples | Stored unencoded at start of each frame |
| Compression ratio | ~2.5x–3.5x | Depends on audio content |

#### LPC Order Selection

```c
// Select LPC order based on signal variance
int select_lpc_order(int32_t *samples, int n) {
    double energy = compute_energy(samples, n);
    if (energy > HIGH_ENERGY_THRESHOLD)  return 32;  // complex signal
    if (energy > MID_ENERGY_THRESHOLD)   return 16;
    return 8;  // simple / near-silence
}
```

---

### 5.2 Perceptual HQ Mode — MDCT + Psychoacoustics

#### Overview

The **Modified Discrete Cosine Transform (MDCT)** converts overlapping audio frames into the frequency domain with 50% overlap-add, eliminating block boundary artifacts. The **psychoacoustic model** computes a masking threshold for each frequency band — below this threshold, quantization noise is inaudible. Coefficients are quantized coarsely in masked bands and finely in unmasked bands, achieving high compression while preserving perceived quality.

#### Algorithm Pipeline

```
Input PCM Frame (1024 samples × 24-bit, 50% overlap)
         │
         ▼
┌──────────────────────────────────┐
│  1. Windowing (KBD or Sine)      │  — applied before MDCT
│     w[n] = KBD_window[n] * x[n] │
└───────────────┬──────────────────┘
                │
                ▼
┌──────────────────────────────────┐
│  2. MDCT                         │  — 1024-point, outputs 512 coeffs
│     X[k] = Σ x[n]·cos(...)       │    via fast algorithm (N log N)
└───────────────┬──────────────────┘
                │
                ▼
┌──────────────────────────────────┐
│  3. Bark-Scale Band Grouping     │  — 64 bands (matches ear resolution)
│     bands[b] = {X[k_start..end]} │    ~1 Bark = ~1 critical bandwidth
└───────────────┬──────────────────┘
                │
                ▼
┌──────────────────────────────────┐
│  4. Psychoacoustic Masking       │  — simultaneous + temporal masking
│     T_mask[b] = f(SPL, spread)   │    spreading function per band
└───────────────┬──────────────────┘
                │
                ▼
┌──────────────────────────────────┐
│  5. Bit Allocation               │  — assign bits per band so
│     bits[b] = g(T_mask[b], E[b]) │    quantization noise < T_mask[b]
└───────────────┬──────────────────┘
                │
                ▼
┌──────────────────────────────────┐
│  6. Quantization                 │  — non-uniform, per-band step size
│     q[k] = round(X[k] / step[b])│
└───────────────┬──────────────────┘
                │
                ▼
┌──────────────────────────────────┐
│  7. Entropy Coding               │  — Huffman tables per band
└───────────────┬──────────────────┘
                │
                ▼
         Compressed bitstream
         (scale factors + quantized + Huffman-coded coefficients)
```

#### Psychoacoustic Masking Model

```
Sound Pressure Level (dB)
    │
100 │      ████                        ← Masker (loud tone at 1kHz)
    │    ████████
 80 │   ██████████
    │  ████████████
 60 │ ██████████████░░░░               ← Masking threshold (spread)
    │████████████████░░░░░░░           ← Anything below threshold is inaudible
 40 │██████████████████░░░░░░░░░       ← Quantization noise can go here
    │████████████████████░░░░░░░░░░
 20 │
    │
  0 └─────────────────────────────────►
    100Hz    1kHz    5kHz   20kHz  Frequency
```

The spreading function models how a loud sound at frequency f masks nearby frequencies, both lower (upward spread is weaker) and higher (downward spread is stronger).

#### Key Parameters

| Parameter | Value | Notes |
|---|---|---|
| Frame size | 1024 samples | ~10.7ms at 96kHz |
| Overlap | 50% (512 samples) | Overlap-add reconstruction |
| MDCT size | 1024-point | 512 output coefficients |
| Frequency bands | 64 Bark-scale | Matches human auditory resolution |
| Masking model | Simultaneous + temporal | Pre-masking 20ms, post-masking 200ms |
| Quantization | Non-uniform per band | 0–15 bits per band |
| Entropy coding | Huffman | Fixed tables v1.0; adaptive in future |
| Target bitrate | 900–1,100 kbps | At 24-bit/96kHz stereo |

---

## 6. Adaptive Bitrate Engine

### 6.1 Design Goal

The ABR engine ensures audio **never drops out** at the cost of quality, and **never wastes quality** when the link can support it. It runs as a background thread polling link statistics every 500ms.

### 6.2 State Machine

```
                    ┌──────────────────────────────────────┐
                    │          ABR State Machine            │
                    └──────────────────────────────────────┘

         RSSI > -65, loss < 1%          RSSI -65 to -75, loss < 3%
         ┌─────────────────────────┐    ┌─────────────────────────┐
         │  STATE_NL_96K           │    │  STATE_NL_48K           │
         │  Near-Lossless 96kHz    │    │  Near-Lossless 48kHz    │
         │  ~1,400 kbps            │    │  ~800 kbps              │
         └────────────┬────────────┘    └────────────┬────────────┘
                      │  link degrades               │
                      ▼                              ▼
         RSSI -75 to -80, loss < 8%    RSSI < -80, loss > 8%
         ┌─────────────────────────┐    ┌─────────────────────────┐
         │  STATE_HQ_96K           │    │  STATE_HQ_48K           │
         │  Perceptual HQ 96kHz    │    │  Perceptual HQ 48kHz    │
         │  ~1,000 kbps            │    │  ~600 kbps              │
         └─────────────────────────┘    └─────────────────────────┘

         Transitions:
         → Upgrade: hysteresis 3s + 3 consecutive good measurements
         → Downgrade: immediate on 2 consecutive bad measurements
```

### 6.3 Hysteresis Logic

```c
#define DOWNGRADE_CONSECUTIVE  2   // downgrade after N bad readings
#define UPGRADE_CONSECUTIVE    3   // upgrade after N good readings
#define UPGRADE_HOLD_MS     3000   // minimum hold before upgrade

void abr_update(ABRCtrl *abr, BTLinkStats *stats) {
    QualityMode target = classify_link(stats);

    if (target < abr->current_mode) {
        // Degraded link — downgrade quickly
        abr->bad_count++;
        abr->good_count = 0;
        if (abr->bad_count >= DOWNGRADE_CONSECUTIVE)
            abr_switch_mode(abr, target);
    } else if (target > abr->current_mode) {
        // Improved link — upgrade cautiously
        abr->good_count++;
        abr->bad_count = 0;
        uint64_t now = get_time_ms();
        if (abr->good_count >= UPGRADE_CONSECUTIVE &&
            now - abr->last_switch_ms >= UPGRADE_HOLD_MS)
            abr_switch_mode(abr, target);
    } else {
        abr->bad_count = 0;
        abr->good_count = 0;
    }
}
```

### 6.4 Mode Switch — Glitch-Free Transition

Mode switches must not cause audible pops or gaps:

```
1. Encoder: mark next frame header with CODEC_CHANGE flag + new mode
2. Encoder: finish current frame in old mode
3. Encoder: begin next frame in new mode
4. Decoder: on receipt of CODEC_CHANGE flag, flush jitter buffer
5. Decoder: reinitialize codec state for new mode
6. Decoder: resume decode — 1 frame gap (21ms), filled with silence
```

---

## 7. OS Integration Layer

### 7.1 Linux — PipeWire Virtual Sink (Laptop B)

AetherCodec registers as a **PipeWire node** with sink semantics. Any application on Laptop B can select "AetherCodec Hi-Res BT" as its audio output device. PipeWire routes those samples to the decoder, which plays them back via the default output (headphone jack).

```
                PipeWire Graph (Laptop B)
                ────────────────────────
  [Spotify / VLC / any app]
         │ samples (24-bit/96kHz)
         ▼
  [PipeWire mixer node]
         │
         ▼
  [AetherCodec Sink Node]  ← registered by aether_receiver
         │ PCM
         ▼
  [AetherCodec Decoder]
         │ PCM
         ▼
  [PipeWire → ALSA → 3.5mm jack DAC]
         │
         ▼
  [Headphones]
```

**PipeWire node properties registered:**

```c
pw_properties_new(
    PW_KEY_NODE_NAME,        "aether_codec_sink",
    PW_KEY_NODE_DESCRIPTION, "AetherCodec Hi-Res BT",
    PW_KEY_MEDIA_TYPE,       "Audio",
    PW_KEY_MEDIA_CATEGORY,   "Playback",
    PW_KEY_MEDIA_ROLE,       "Music",
    PW_KEY_NODE_LATENCY,     "2048/96000",   // 21ms at 96kHz
    NULL
)
```

### 7.2 Linux — PipeWire Source Capture (Laptop A)

The sender captures system audio from a PipeWire **source node** — either a specific application stream or the monitor of the system output.

```
[Music Player on Laptop A]
         │
         ▼
  [PipeWire source monitor]
         │ PCM 24-bit/96kHz
         ▼
  [AetherCodec Encoder] → RFCOMM → Laptop B
```

### 7.3 ALSA Fallback Plugin

For systems without PipeWire, AetherCodec provides an ALSA plugin (`libasound_module_pcm_aether.so`) that creates a virtual ALSA PCM device.

```
# ~/.asoundrc
pcm.aether {
    type aether
    target_addr "AA:BB:CC:DD:EE:FF"   # Laptop B Bluetooth address
    channel 1
    sample_rate 96000
    bit_depth 24
    mode near_lossless
}
```

### 7.4 Windows — WASAPI Virtual Device (Phase 2)

Deferred to Phase 2. Approach:
- Implement as a WDM kernel streaming (KS) filter
- Register as an audio endpoint via Windows Audio Device Graph
- Expose via WASAPI exclusive mode for minimum latency

---

## 8. Packet Format Specification

### 8.1 AetherPacket Structure

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
├─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┴─┤
│                       Magic: 0xAE7HC0DE                          │  bytes 0–3
├─────────────────────────────────────────────────────────────────┤
│                   Sequence Number (uint32)                       │  bytes 4–7
├─────────────────────────────────────────────────────────────────┤
│                   Timestamp in µs (uint32)                      │  bytes 8–11
├───────────────┬───────────────┬───────────────┬─────────────────┤
│ Mode (uint8)  │ Sample Rate   │ Bit Depth     │ Channels        │  bytes 12–15
│ 0=NL, 1=HQ   │ (uint8) *     │ 16 or 24      │ 1 or 2          │
├───────────────┴───────────────┴───────────────┴─────────────────┤
│               Payload Size in bytes (uint16)                     │  bytes 16–17
├─────────────────────────────────────────────────────────────────┤
│                   Header CRC-16 (uint16)                        │  bytes 18–19
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│                   Compressed Payload                             │  bytes 20–(20+N)
│                   (N = Payload Size)                             │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│                   Payload CRC-32 (uint32)                       │  bytes 20+N – 20+N+3
└─────────────────────────────────────────────────────────────────┘

* Sample Rate field encoding:
  0x00 = 44,100 Hz
  0x01 = 48,000 Hz
  0x02 = 88,200 Hz
  0x03 = 96,000 Hz

Total header size: 24 bytes
Maximum payload:  65,535 bytes (uint16 limit) — fragmented if > RFCOMM MTU
```

### 8.2 Control Packet Types

Control packets use the same header with `Mode = 0xFF` and specific magic payloads:

| Control Type | Payload | Purpose |
|---|---|---|
| `CTRL_HANDSHAKE` | Protocol version + capabilities | Initial connection setup |
| `CTRL_CODEC_CHANGE` | New mode + new sample rate | ABR-triggered mode switch |
| `CTRL_STATS_REQUEST` | — | Request link stats from receiver |
| `CTRL_STATS_REPLY` | RSSI + packet loss % + buffer level | Receiver reports back to sender |
| `CTRL_TEARDOWN` | Reason code | Graceful disconnect |

### 8.3 Handshake Sequence

```
Sender                              Receiver
──────                              ────────
CTRL_HANDSHAKE (version=1,          
  caps: NL|HQ|96k|48k|44k)  ──►    validate version
                               ◄──  CTRL_HANDSHAKE_ACK (agreed caps)
[begin audio stream]
AetherPacket (seq=0) ──────────►    [begin decode + playback]
AetherPacket (seq=1) ──────────►
...
                               ◄──  CTRL_STATS_REPLY (every 500ms)
[ABR adjusts if needed]
CTRL_CODEC_CHANGE ─────────────►    [flush, reinit]
AetherPacket (seq=N, new mode) ►
```

---

## 9. Data Flow Diagrams

### 9.1 Encoding Path (Laptop A)

```
PipeWire Source
     │
     │  interleaved S24_3LE PCM samples
     ▼
[capture_thread: ring buffer write]
     │
     │  PCM frames (2048 or 1024 samples)
     ▼
[encode_thread]
     │
     ├─ if MODE_NL ──► lpc_encode_frame()
     │                      │
     │                      ├── compute_autocorr()
     │                      ├── levinson_durbin()
     │                      ├── compute_residuals()
     │                      └── rice_encode()
     │                              │
     └─ if MODE_HQ ──► mdct_encode_frame()
                            │
                            ├── apply_window()
                            ├── mdct_transform()
                            ├── bark_group_bands()
                            ├── compute_masking()
                            ├── allocate_bits()
                            ├── quantize_bands()
                            └── huffman_encode()
                                    │
                                    ▼
                         [pack_aether_packet()]
                                    │
                                    ▼
                         [send_thread: RFCOMM write]
```

### 9.2 Decoding Path (Laptop B)

```
RFCOMM socket recv
     │
     │  raw bytes
     ▼
[recv_thread]
     │
     ├── fragment reassembly
     ├── CRC validation
     └── unpack_aether_packet()
               │
               ▼
     [jitter_buf_insert()]
               │
               │  ordered AetherPackets
               ▼
     [decode_thread: jitter_buf_pop() at 96kHz clock]
               │
               ├─ if MODE_NL ──► lpc_decode_frame()
               │                      │
               │                      ├── rice_decode()
               │                      └── lpc_synthesis()
               │
               └─ if MODE_HQ ──► mdct_decode_frame()
                                       │
                                       ├── huffman_decode()
                                       ├── dequantize_bands()
                                       ├── inverse_mdct()
                                       └── overlap_add()
                                               │
                                               ▼
                                    [PipeWire sink write]
                                               │
                                               ▼
                                    [3.5mm DAC → Headphones]
```

---

## 10. Module Interface Contracts

### 10.1 `aether_encoder.h`

```c
typedef struct AetherEncoder AetherEncoder;

// Create encoder
// mode: AETHER_MODE_NL or AETHER_MODE_HQ
// sample_rate: 44100, 48000, 88200, 96000
// bit_depth: 16 or 24
// channels: 1 or 2
AetherEncoder* aether_encoder_create(int mode, int sample_rate,
                                      int bit_depth, int channels);

// Encode one frame of PCM samples
// pcm_in: interleaved samples, frame_samples count
// pkt_out: caller-allocated AetherPacket buffer
// Returns: bytes written to pkt_out, or -1 on error
int aether_encoder_encode(AetherEncoder *enc, const int32_t *pcm_in,
                           int frame_samples, AetherPacket *pkt_out);

// Switch mode mid-stream (ABR callback)
// Takes effect from next frame
void aether_encoder_set_mode(AetherEncoder *enc, int new_mode);

void aether_encoder_destroy(AetherEncoder *enc);
```

### 10.2 `aether_decoder.h`

```c
typedef struct AetherDecoder AetherDecoder;

AetherDecoder* aether_decoder_create(void);  // auto-detects mode from packets

// Decode one AetherPacket
// pcm_out: caller-allocated, must hold max_samples int32_t values
// Returns: samples written, or -1 on error
int aether_decoder_decode(AetherDecoder *dec, const AetherPacket *pkt,
                           int32_t *pcm_out, int max_samples);

void aether_decoder_flush(AetherDecoder *dec);   // call on mode switch
void aether_decoder_destroy(AetherDecoder *dec);
```

### 10.3 `jitter_buf.h`

```c
typedef struct JitterBuf JitterBuf;

// target_ms: desired playout delay (recommended: 40ms)
JitterBuf* jitter_buf_create(int target_ms, int sample_rate);

// Insert received packet (can be out of order)
void jitter_buf_insert(JitterBuf *jb, const AetherPacket *pkt);

// Pop next frame for playout (blocks until due time)
// Returns: pointer to packet, or NULL if packet lost (conceal)
const AetherPacket* jitter_buf_pop(JitterBuf *jb);

// Get current buffer level in ms (for stats reporting)
int jitter_buf_level_ms(const JitterBuf *jb);

void jitter_buf_destroy(JitterBuf *jb);
```

### 10.4 `abr_ctrl.h`

```c
typedef struct ABRCtrl ABRCtrl;
typedef void (*ABRCallback)(int new_mode, int new_sample_rate, void *userdata);

ABRCtrl* abr_ctrl_create(ABRCallback cb, void *userdata);

// Feed link stats (called by stats_thread every 500ms)
void abr_update(ABRCtrl *abr, int rssi_dbm, float packet_loss_pct,
                int jitter_buf_level_ms);

void abr_ctrl_destroy(ABRCtrl *abr);
```

---

## 11. Memory & Threading Model

### 11.1 Thread Overview

```
Process: aether_sender (Laptop A)
──────────────────────────────────
Thread              Priority    Blocked on
capture_thread      HIGH        PipeWire callback
encode_thread       HIGH        capture ring buffer
send_thread         NORMAL      encode queue + RFCOMM socket
bitrate_ctrl_thread LOW         500ms timer + stats socket
```

```
Process: aether_receiver (Laptop B)
─────────────────────────────────────
Thread              Priority    Blocked on
recv_thread         HIGH        RFCOMM socket recv
decode_thread       HIGH        jitter buffer pop (timed)
playback_thread     HIGH        PipeWire callback
stats_thread        LOW         500ms timer + stats request
```

### 11.2 Ring Buffer Design

Between capture and encode threads, and between decode and playback:

```c
// Lock-free single-producer single-consumer ring buffer
typedef struct {
    int32_t  *data;           // sample storage
    uint32_t  capacity;       // in samples
    atomic_uint head;         // write pointer (capture/decode writes)
    atomic_uint tail;         // read pointer  (encode/playback reads)
} AudioRingBuf;
```

SPSC ring buffer avoids mutex overhead on the hot audio path.

### 11.3 Memory Allocation Strategy

| Component | Strategy | Notes |
|---|---|---|
| Audio ring buffers | `mmap` at startup | Fixed size, no runtime alloc on hot path |
| AetherPacket pool | Pre-allocated pool of 64 packets | Avoids malloc in encode/decode loop |
| MDCT work buffers | Stack-allocated per frame | 1024 × 8 bytes = 8KB, fits L1 cache |
| LPC coefficients | Stack-allocated per frame | order × 8 bytes |
| Jitter buffer | Fixed array of 128 packet slots | Enough for 128 × 21ms = 2.7s max depth |

---

## 12. Error Handling Strategy

### 12.1 Packet Loss Concealment

When `jitter_buf_pop()` returns NULL (packet lost or too late):

```
Strategy 1 — Silence insertion:
  Output a frame of silence (zeros). Simple, avoids artifacts.
  Used when: loss rate > 10% (signal too degraded to predict)

Strategy 2 — Linear interpolation:
  Interpolate between last good frame's final samples and next good frame's first samples.
  Used when: isolated single packet loss (<5% loss rate)

Strategy 3 — LPC-based extrapolation (NL mode only):
  Reuse the last frame's LPC coefficients to extrapolate forward.
  Used when: up to 3 consecutive lost packets, NL mode
```

### 12.2 CRC Failure Handling

```
Header CRC fail → discard packet entirely, log error, do not pass to decoder
Payload CRC fail → discard payload, use concealment for that frame
                   (header is intact so sequence number is still known)
```

### 12.3 Transport Errors

| Error | Action |
|---|---|
| RFCOMM `send()` returns -1 | Log, retry once, then pause 5ms and retry |
| RFCOMM connection dropped | Attempt reconnect 3 times, then exit with error |
| Encoder returns -1 | Skip frame, insert silence, log |
| PipeWire callback overrun | Drop oldest frames from ring buffer, log |

---

## 13. Build System & Directory Layout

### 13.1 Directory Structure

```
aethercodec/
├── CMakeLists.txt
├── README.md
├── HLD.md                         ← this document
├── include/
│   ├── aether_encoder.h
│   ├── aether_decoder.h
│   ├── aether_packet.h
│   ├── jitter_buf.h
│   ├── abr_ctrl.h
│   ├── codec_lpc.h
│   └── codec_mdct.h
├── src/
│   ├── encoder/
│   │   ├── aether_encoder.c
│   │   ├── codec_lpc_enc.c        ← LPC + Rice encoder
│   │   └── codec_mdct_enc.c       ← MDCT + psychoacoustic encoder
│   ├── decoder/
│   │   ├── aether_decoder.c
│   │   ├── codec_lpc_dec.c        ← LPC synthesis + Rice decoder
│   │   └── codec_mdct_dec.c       ← MDCT inverse + Huffman decoder
│   ├── transport/
│   │   ├── transport_rfcomm.c     ← BlueZ RFCOMM socket layer
│   │   └── aether_packet.c        ← Pack/unpack + CRC
│   ├── abr/
│   │   └── abr_ctrl.c             ← Adaptive bitrate state machine
│   ├── jitter/
│   │   └── jitter_buf.c           ← Jitter buffer + concealment
│   ├── os/
│   │   ├── pw_sink.c              ← PipeWire virtual sink (Laptop B)
│   │   ├── pw_source.c            ← PipeWire source capture (Laptop A)
│   │   └── alsa_plugin.c          ← ALSA fallback
│   └── daemon/
│       ├── aether_sender.c        ← main() for Laptop A
│       └── aether_receiver.c      ← main() for Laptop B
├── tools/
│   ├── aether_analyze.c           ← Spectral analysis + quality report
│   └── aether_bench.c             ← Encoder throughput benchmarking
└── tests/
    ├── test_lpc.c
    ├── test_mdct.c
    ├── test_packet.c
    └── test_jitter.c
```

### 13.2 Build Commands

```bash
# Configure
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DAETHER_ENABLE_PIPEWIRE=ON \
         -DAETHER_ENABLE_ALSA=ON

# Build
make -j$(nproc)

# Run tests
make test

# Install system-wide (makes aetherd available in PATH)
sudo make install

# Usage — Laptop B (start receiver first)
aetherd receive --mode auto --sample-rate 96000 --bit-depth 24

# Usage — Laptop A (connect to Laptop B's BT address)
aetherd send --target AA:BB:CC:DD:EE:FF --mode near-lossless --input pipewire
```

---

## 14. Key Design Decisions & Rationale

### Why RFCOMM instead of A2DP?

A2DP's codec negotiation is fixed to registered profiles. Injecting a custom codec requires a kernel Bluetooth driver modification, which is impractical for a student project. RFCOMM gives a raw byte stream over the same Bluetooth radio with nearly identical throughput, and the codec is entirely userspace.

### Why LPC for Near-Lossless instead of raw FLAC?

FLAC is an excellent reference but its library API adds overhead and its framing is not optimized for low-latency streaming (FLAC frames are variable size, requiring buffering). A direct LPC implementation gives full control over frame timing, warm-up sample handling, and integration with the ABR engine.

### Why MDCT instead of FFT for the perceptual mode?

MDCT with 50% overlap-add eliminates block boundary artifacts (pre-echo and ringing) that FFT-based coders suffer from. This is why all modern perceptual codecs (AAC, Opus, Vorbis, MP3) use MDCT or a variant of it.

### Why 96kHz and not 192kHz?

At 192kHz, even the perceptual HQ mode would need ~2,000 kbps, which exceeds practical Bluetooth EDR bandwidth. 96kHz at 24-bit is the "hi-res" threshold that is both physically achievable and audibly meaningful — human hearing extends to ~20kHz, so 96kHz captures everything with a comfortable anti-aliasing margin.

### Why 2048-sample frames for NL mode and 1024 for HQ mode?

Larger frames give LPC more context for prediction (better compression) and are worth the extra latency since NL mode is for high-quality listening, not gaming. HQ (MDCT) mode uses 1024 samples because the psychoacoustic model's temporal masking window aligns naturally with ~10ms frames, and halving the frame size halves latency.

### Why not use Opus directly?

Opus is an excellent codec but it is lossy by design, targets speech and music at 6–510 kbps, and has no lossless mode. AetherCodec's NL mode addresses a use case Opus explicitly does not.

---

## 15. Limitations & Known Constraints

| Limitation | Root Cause | Mitigation |
|---|---|---|
| Max ~1,400 kbps on BT EDR | Physical Bluetooth bandwidth ceiling | NL mode targets 24/96 at 1,400 kbps; falls back gracefully |
| No real headset support yet | Headset chip firmware cannot decode AetherCodec | Phase 3: ESP32 or QCC dev kit port |
| Linux-only in Phase 1 | Windows audio driver complexity | Phase 2: WASAPI virtual device |
| Latency 40–80ms | Jitter buffer + encode/decode pipeline | Not a gaming codec; target is music listening |
| No DRM / content protection | Open codec, open stream | By design — this is an open project |
| RFCOMM not standard audio | No interoperability with unmodified devices | Requires AetherCodec on both ends |
| No stereo channel independence | Joint stereo only in v1.0 | Mid-side stereo in v1.1 roadmap |

---

*AetherCodec HLD v1.0 — Open Source — Zero Budget — Built in C*  
*This document is the technical reference for all implementation work. Update this document when any interface contract, data format, or architectural decision changes.*
