# AetherCodec — Project Summary

## The Problem It Solves

Standard Bluetooth audio on Linux is built entirely around A2DP, and every codec available under that profile — SBC, AAC, aptX, even LDAC where supported — is fundamentally a lossy, bandwidth-constrained format. This creates a stack of compounding problems for anyone trying to move real hi-res audio (24-bit/96kHz) wirelessly between two Linux machines:

- **No bit-perfect option exists over Bluetooth today.** Every mainstream A2DP codec throws away information to hit its target bitrate (typically 320 kbps–990 kbps). There is no standard, open way to get a lossless wireless audio path on Linux — proprietary formats like LDAC get closest but are neither open nor guaranteed bit-exact.
- **The bitrate math doesn't work out even if you wanted lossless.** A 24-bit/96kHz stereo lossless stream needs roughly 3+ Mbps; A2DP caps out an order of magnitude below that. AetherCodec's own measurements put real NL-mode traffic at ~3.2 Mbps against a Bluetooth RFCOMM link that only reliably carries ~200 kbps — which is precisely why the project treats classic Bluetooth as insufficient on its own and adds a Wi-Fi/TCP transport as the path that actually delivers hi-res audio, while Bluetooth stays viable for the perceptual/lossy mode.
- **Linux has no first-class hi-res audio driver story.** Vendor codecs (aptX HD, LDAC) are gated behind licensing and driver support that's inconsistent-to-absent on Linux; A2DP support in the kernel/BlueZ stack targets "good enough" consumer audio, not audiophile or professional use.
- **Existing solutions don't gracefully degrade.** Consumer Bluetooth codecs pick one bitrate and hope the link holds; when RSSI drops or interference hits, you get dropouts or the OS silently renegotiates to a worse codec. There's no visibility or control over why audio quality changed.
- **No open, inspectable codec exists for this niche.** The few proprietary options that get close to hi-res wireless audio are closed-source, license-encumbered, and impossible to adapt, extend, or run analysis on.

AetherCodec's answer is to sidestep A2DP entirely and build a purpose-made pipeline:

1. Stream raw framed audio over RFCOMM, L2CAP, or TCP/Wi-Fi instead of a standard Bluetooth audio profile.
2. Offer two runtime-selectable modes tuned for different priorities:
   - **Near-Lossless (NL)** — LPC + Rice coding, bit-perfect round-trip, for when you want your audio exactly as captured.
   - **Perceptual HQ** — MDCT + psychoacoustic masking, transparent-quality but far lower bitrate, for constrained links.
3. Run an adaptive-bitrate engine that watches RSSI and real packet loss/congestion and steps down the quality ladder before audio actually drops, then climbs back up as headroom returns — so degradation is a controlled, audible-last-resort rather than silent dropouts.
4. Do all of this with zero paid dependencies, fully open source, so the codec, the transport, and the ABR logic are all inspectable and modifiable — filling a gap where no open, hi-res-capable, Linux-native wireless audio codec currently exists.

In short: it gives Linux users a way to get either truly lossless or transparently-perceptual audio wirelessly between two machines, a capability that simply isn't available through any standard Bluetooth audio profile today, with an adaptive system that keeps the stream alive and audible as real-world link conditions change.

## Challenges We Ran Into

- **Bluetooth Classic can't actually carry the audio.** Measured NL-96k needs ~3.2 Mbps; RFCOMM realistically delivers ~200 kbps. This is why TCP-over-Wi-Fi transport (`--tcp`) was added as the transport that actually makes NL/HQ usable, while RFCOMM/L2CAP remain for genuine short-range Bluetooth testing.
- **Bit-perfect losslessness is unforgiving.** The NL path (LPC + Rice coding) requires encoder and decoder to run identical integer arithmetic over identical history — any drift (rounding, a Q15 mismatch, `wasted`-bits handling) breaks losslessness rather than just degrading quality. Several documented "intentional divergences" from the spec (e.g. `LPC_RICE_MAX_PARAM=30` instead of the spec's 15, which overflowed the unary code on noisy signal) came directly from debugging this invariant.
- **The reference docs (PRD/HLD) had real errors**, not just omissions — a non-existent magic-number hex value, a backwards ABR quality-ordering rule that would upgrade quality on link failure, an MDCT sketch that called non-existent FFTW functions, and a virtual-sink placement that contradicted itself between steps. Each had to be diagnosed and consciously overridden rather than followed.
- **PipeWire's virtual-sink integration was fragile.** Marking the sink `node.driver=true` seemed reasonable but silently stalled the whole graph (stream never left `streaming` state). Buffer sizing also had to be pinned to one quantum — filling PipeWire's `maxsize` over-drained the ring buffer and caused underruns that looked like packet loss but weren't.
- **Adaptive bitrate tuning required chasing symptoms across the whole pipeline.** A naive "optimistic start at NL-96k" flooded the queue and cascaded down to the worst rung in the first second of every real session; queue-based backpressure (not just RSSI) had to be added as a second congestion signal; and switching rungs without flushing the stale backlog made the new rung look congested too, creating a feedback loop.
- **Encode/send coupling caused audio stutter.** Blocking the capture/encode loop on `rfcomm_send_packet` during a slow link dropped captured audio. This forced decoupling encode from send via a bounded queue, sized by queued audio time rather than packet count (since NL and HQ frames differ ~4x in duration).
- **Real two-laptop testing is inherently harder to develop against.** RSSI polling, RFCOMM negotiation, and real packet loss can't be fully exercised on one machine, so a `--loopback` mode had to be built just to validate the capture→encode→decode→playback pipeline in single-machine development.

## Technologies Used

- **Language / Build:** C11, CMake, CTest
- **Bluetooth:** BlueZ, raw RFCOMM sockets, experimental L2CAP `SOCK_SEQPACKET`, HCI (RSSI polling)
- **Networking:** Raw TCP sockets (Wi-Fi transport fallback)
- **Audio I/O:** PipeWire (virtual sink + playback client), optional ALSA fallback
- **DSP:**
  - Custom LPC (linear predictive coding) + Rice/Golomb entropy coding for the lossless mode
  - Custom MDCT (fold + DCT-IV via FFTW's single-precision `fftw3f`) + psychoacoustic masking (Bark bands, absolute threshold of hearing, spreading function) for the perceptual mode
- **Custom transport layer:** hand-rolled packet framing with CRC16/CRC32 integrity checks
- **Systems components built from scratch:** SPSC audio ring buffer, jitter buffer (reorder + loss detection), adaptive-bitrate controller (ladder + hysteresis + AIMD-style step control)
