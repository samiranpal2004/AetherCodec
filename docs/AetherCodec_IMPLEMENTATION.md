# AetherCodec — Step-by-Step Implementation Plan

> **Version:** 1.0 | **Date:** July 2026  
> **Read before coding:** Complete HLD.md and PRD.md first.  
> **Language:** C (C11 standard)  
> **Platform:** Linux (Ubuntu 22.04+ / Arch with PipeWire)  
> **Hardware:** 2 laptops with Bluetooth 4.0+ and 3.5mm headphone jack

---

## How to Use This Document

Each step has:
- **What** — exactly what to build
- **How** — code, commands, file paths
- **Verify** — how to confirm it works before moving on
- **Checkpoint** — never proceed past a checkpoint until it passes

> ⚠️ **Rule:** Never skip a Verify step. A broken foundation wastes everything built on top of it.

---

## Table of Contents

- [Phase 0 — Environment Setup](#phase-0--environment-setup)
- [Phase 1 — Transport Layer](#phase-1--transport-layer)
- [Phase 2 — Near-Lossless Codec (LPC + Rice)](#phase-2--near-lossless-codec-lpc--rice)
- [Phase 3 — Perceptual HQ Codec (MDCT + Psychoacoustics)](#phase-3--perceptual-hq-codec-mdct--psychoacoustics)
- [Phase 4 — OS Integration (PipeWire)](#phase-4--os-integration-pipewire)
- [Phase 5 — Adaptive Bitrate Engine](#phase-5--adaptive-bitrate-engine)
- [Phase 6 — End-to-End Demo & Measurement](#phase-6--end-to-end-demo--measurement)

---

## Phase 0 — Environment Setup

> **Goal:** Both laptops have all required tools installed and Bluetooth pairing works between them.

---

### Step 0.1 — Install Dependencies on Both Laptops

Run on **both Laptop A and Laptop B**:

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    libbluetooth-dev \
    bluez \
    bluez-tools \
    pipewire \
    pipewire-audio \
    libpipewire-0.3-dev \
    libasound2-dev \
    libfftw3-dev \
    sox \
    audacity \
    git \
    valgrind \
    clang \
    gdb

# Arch Linux
sudo pacman -S --needed \
    base-devel cmake pkgconf bluez bluez-libs bluez-utils \
    pipewire pipewire-audio wireplumber \
    alsa-lib fftw sox audacity git valgrind clang gdb
```

**Verify:**
```bash
gcc --version        # should show GCC 11+
bluetoothctl --version
pkg-config --modversion libpipewire-0.3
pkg-config --modversion bluez
```

---

### Step 0.2 — Verify Bluetooth Hardware on Both Laptops

```bash
# Check BT adapter exists
hciconfig -a
# Should show hci0 with BD Address

# Check BT daemon is running
systemctl status bluetooth

# If not running:
sudo systemctl enable --now bluetooth
```

**Verify:**
```bash
hciconfig hci0 | grep "BD Address"
# Should print something like: BD Address: AA:BB:CC:DD:EE:FF
```

Write down both laptop BT addresses. You will use them constantly.

---

### Step 0.3 — Pair the Two Laptops

On **Laptop B** (will be receiver):
```bash
bluetoothctl
  power on
  agent on
  discoverable on
  pairable on
```

On **Laptop A** (will be sender):
```bash
bluetoothctl
  power on
  agent on
  scan on
  # Wait for Laptop B to appear, note its address (e.g. AA:BB:CC:DD:EE:FF)
  pair AA:BB:CC:DD:EE:FF
  trust AA:BB:CC:DD:EE:FF
  connect AA:BB:CC:DD:EE:FF
```

**Verify:**
```bash
# On Laptop A
bluetoothctl info AA:BB:CC:DD:EE:FF
# Should show: Paired: yes, Trusted: yes, Connected: yes
```

---

### Step 0.4 — Set Up Project Directory Structure

Run on **both laptops** (clone from the same git repo):

```bash
mkdir -p ~/aethercodec
cd ~/aethercodec
git init

mkdir -p src/{encoder,decoder,transport,abr,jitter,os,daemon}
mkdir -p include tests tools build

# Create placeholder files
touch include/aether_encoder.h
touch include/aether_decoder.h
touch include/aether_packet.h
touch include/jitter_buf.h
touch include/abr_ctrl.h
touch include/codec_lpc.h
touch include/codec_mdct.h
touch CMakeLists.txt
```

---

### Step 0.5 — Create Base CMakeLists.txt

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(aethercodec C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -fsanitize=address")

find_package(PkgConfig REQUIRED)
pkg_check_modules(BLUEZ REQUIRED bluez)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(FFTW REQUIRED fftw3)

include_directories(include)
include_directories(${BLUEZ_INCLUDE_DIRS})
include_directories(${PIPEWIRE_INCLUDE_DIRS})

add_subdirectory(src)
add_subdirectory(tests)
```

**Verify:**
```bash
cd ~/aethercodec
mkdir build && cd build
cmake ..
# Should complete without errors (warnings about empty src/ are fine at this stage)
```

---

### ✅ Phase 0 Checkpoint

- [x] Packages installed (Laptop A / this machine) — Ubuntu 26.04 LTS, bluez 5.85,
      libpipewire-0.3 1.6.2, fftw3f 3.3.10, gcc 15.2.0, cmake 4.2.3
- [x] `hciconfig hci0` shows a BD Address (C0:35:32:24:8E:D0) — this machine
- [x] `hci0` is UP — `UP RUNNING PSCAN`, rfkill unblocked (adapter: Realtek,
      HCI 5.2, name `sudipta-LOQ-15IRX9`)
- [ ] Laptops are paired and trusted with each other — pending second laptop
- [x] `cmake ..` runs without errors
- [x] Project directory structure exists
- [x] Full build succeeds (`aether_transport`, `aether_codec`, all tools) and
      `ctest`: 3/3 tests passed (`test_packet`, `test_lpc`, `test_codec_nl`)

> **Note:** an earlier revision of this checkpoint recorded a different BD
> address (`5C:F3:70:6A:0F:CB`) and older package versions — that was a
> different environment, not this machine. Replaced above with this machine's
> actual verified values now that it's designated **Laptop A**.

---

## Phase 1 — Transport Layer

> **Goal:** Stream raw bytes from Laptop A to Laptop B over Bluetooth RFCOMM. No audio yet — just prove the pipe works and measure throughput and latency.

---

### Step 1.1 — Define the Packet Header

Create `include/aether_packet.h`:

```c
#ifndef AETHER_PACKET_H
#define AETHER_PACKET_H

#include <stdint.h>
#include <stddef.h>

#define AETHER_MAGIC        0xAE7EC0DE
#define AETHER_HEADER_SIZE  24
#define AETHER_MAX_PAYLOAD  65535
#define AETHER_RFCOMM_MTU   672   // typical RFCOMM MTU

/* Codec modes */
#define AETHER_MODE_NL      0x00  // Near-Lossless (LPC + Rice)
#define AETHER_MODE_HQ      0x01  // High Quality Perceptual (MDCT)
#define AETHER_MODE_CTRL    0xFF  // Control packet

/* Sample rate codes */
#define AETHER_RATE_44100   0x00
#define AETHER_RATE_48000   0x01
#define AETHER_RATE_88200   0x02
#define AETHER_RATE_96000   0x03

/* Control packet subtypes (stored in payload[0]) */
#define CTRL_HANDSHAKE      0x01
#define CTRL_HANDSHAKE_ACK  0x02
#define CTRL_CODEC_CHANGE   0x03
#define CTRL_STATS_REQUEST  0x04
#define CTRL_STATS_REPLY    0x05
#define CTRL_TEARDOWN       0x06

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         // always AETHER_MAGIC
    uint32_t sequence;      // packet sequence number
    uint32_t timestamp_us;  // sender timestamp in microseconds
    uint8_t  mode;          // AETHER_MODE_*
    uint8_t  sample_rate;   // AETHER_RATE_*
    uint8_t  bit_depth;     // 16 or 24
    uint8_t  channels;      // 1 or 2
    uint16_t payload_size;  // bytes in payload
    uint16_t header_crc16;  // CRC-16 of bytes 0..17
} AetherHeader;
#pragma pack(pop)

typedef struct {
    AetherHeader hdr;
    uint8_t  payload[AETHER_MAX_PAYLOAD];
    uint32_t payload_crc32; // CRC-32 of payload bytes
} AetherPacket;

/* Utility functions */
uint16_t aether_crc16(const uint8_t *data, size_t len);
uint32_t aether_crc32(const uint8_t *data, size_t len);

int  aether_packet_pack(const AetherPacket *pkt, uint8_t *buf, size_t buf_size);
int  aether_packet_unpack(const uint8_t *buf, size_t buf_len, AetherPacket *pkt_out);
int  aether_packet_validate(const AetherPacket *pkt);

uint64_t aether_timestamp_us(void);  // monotonic clock in microseconds

#endif /* AETHER_PACKET_H */
```

---

### Step 1.2 — Implement CRC and Packet Pack/Unpack

Create `src/transport/aether_packet.c`:

```c
#include "aether_packet.h"
#include <string.h>
#include <time.h>

/* CRC-16/CCITT-FALSE */
uint16_t aether_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

/* CRC-32/ISO-HDLC */
uint32_t aether_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
    }
    return crc ^ 0xFFFFFFFF;
}

uint64_t aether_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

/* Serialize packet to byte buffer for sending over RFCOMM.
   Returns total bytes written, or -1 on error. */
int aether_packet_pack(const AetherPacket *pkt, uint8_t *buf, size_t buf_size) {
    size_t total = AETHER_HEADER_SIZE + pkt->hdr.payload_size + 4;
    if (buf_size < total) return -1;

    /* Copy header (compute CRC on all fields except header_crc16 itself) */
    AetherHeader hdr = pkt->hdr;
    hdr.header_crc16 = 0;
    memcpy(buf, &hdr, AETHER_HEADER_SIZE - 2);  // everything before crc16
    uint16_t hcrc = aether_crc16(buf, AETHER_HEADER_SIZE - 2);
    hdr.header_crc16 = hcrc;
    memcpy(buf, &hdr, AETHER_HEADER_SIZE);

    /* Copy payload */
    memcpy(buf + AETHER_HEADER_SIZE, pkt->payload, pkt->hdr.payload_size);

    /* Append payload CRC-32 */
    uint32_t pcrc = aether_crc32(pkt->payload, pkt->hdr.payload_size);
    memcpy(buf + AETHER_HEADER_SIZE + pkt->hdr.payload_size, &pcrc, 4);

    return (int)total;
}

/* Deserialize buffer into AetherPacket.
   Returns 0 on success, -1 on bad magic, -2 on bad header CRC, -3 on bad payload CRC. */
int aether_packet_unpack(const uint8_t *buf, size_t buf_len, AetherPacket *out) {
    if (buf_len < AETHER_HEADER_SIZE) return -1;

    memcpy(&out->hdr, buf, AETHER_HEADER_SIZE);

    if (out->hdr.magic != AETHER_MAGIC) return -1;

    /* Validate header CRC */
    uint16_t stored_hcrc = out->hdr.header_crc16;
    out->hdr.header_crc16 = 0;
    uint16_t computed_hcrc = aether_crc16(buf, AETHER_HEADER_SIZE - 2);
    out->hdr.header_crc16 = stored_hcrc;
    if (stored_hcrc != computed_hcrc) return -2;

    size_t expected = AETHER_HEADER_SIZE + out->hdr.payload_size + 4;
    if (buf_len < expected) return -1;

    memcpy(out->payload, buf + AETHER_HEADER_SIZE, out->hdr.payload_size);

    /* Validate payload CRC */
    uint32_t stored_pcrc;
    memcpy(&stored_pcrc, buf + AETHER_HEADER_SIZE + out->hdr.payload_size, 4);
    uint32_t computed_pcrc = aether_crc32(out->payload, out->hdr.payload_size);
    if (stored_pcrc != computed_pcrc) return -3;

    return 0;
}
```

**Test CRC immediately:**
```c
// tests/test_packet.c
#include "aether_packet.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

int main(void) {
    AetherPacket pkt = {0};
    pkt.hdr.magic        = AETHER_MAGIC;
    pkt.hdr.sequence     = 42;
    pkt.hdr.timestamp_us = aether_timestamp_us();
    pkt.hdr.mode         = AETHER_MODE_NL;
    pkt.hdr.sample_rate  = AETHER_RATE_96000;
    pkt.hdr.bit_depth    = 24;
    pkt.hdr.channels     = 2;

    const char *msg = "Hello AetherCodec";
    memcpy(pkt.payload, msg, strlen(msg));
    pkt.hdr.payload_size = (uint16_t)strlen(msg);

    uint8_t buf[4096];
    int n = aether_packet_pack(&pkt, buf, sizeof(buf));
    assert(n > 0);

    AetherPacket unpacked = {0};
    int r = aether_packet_unpack(buf, n, &unpacked);
    assert(r == 0);
    assert(unpacked.hdr.sequence == 42);
    assert(unpacked.hdr.magic == AETHER_MAGIC);
    assert(memcmp(unpacked.payload, msg, strlen(msg)) == 0);

    /* Test corruption detection */
    buf[30] ^= 0xFF;  // corrupt payload
    r = aether_packet_unpack(buf, n, &unpacked);
    assert(r == -3);  // payload CRC should fail

    printf("✓ Packet pack/unpack/CRC: PASS\n");
    return 0;
}
```

```bash
cd build && cmake .. && make test_packet && ./tests/test_packet
```

---

### Step 1.3 — Implement RFCOMM Transport

Create `include/transport_rfcomm.h`:

```c
#ifndef TRANSPORT_RFCOMM_H
#define TRANSPORT_RFCOMM_H

#include "aether_packet.h"

#define RFCOMM_CHANNEL  1

typedef struct RFCOMMTransport RFCOMMTransport;

/* Server side (Laptop B — receiver) */
RFCOMMTransport* rfcomm_listen(uint8_t channel);   // blocks until connected
void             rfcomm_server_close(RFCOMMTransport *t);

/* Client side (Laptop A — sender) */
RFCOMMTransport* rfcomm_connect(const char *target_addr, uint8_t channel);
void             rfcomm_client_close(RFCOMMTransport *t);

/* Send/receive packets (blocking) */
int  rfcomm_send_packet(RFCOMMTransport *t, const AetherPacket *pkt);
int  rfcomm_recv_packet(RFCOMMTransport *t, AetherPacket *pkt_out);

/* Get raw socket fd (for select/poll) */
int  rfcomm_get_fd(const RFCOMMTransport *t);

#endif
```

Create `src/transport/transport_rfcomm.c`:

```c
#include "transport_rfcomm.h"
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct RFCOMMTransport {
    int server_fd;   // listening socket (-1 for client)
    int conn_fd;     // active connection socket
    uint8_t tx_buf[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4 + 16];
    uint8_t rx_buf[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4 + 16];
};

RFCOMMTransport* rfcomm_listen(uint8_t channel) {
    RFCOMMTransport *t = calloc(1, sizeof(*t));
    t->server_fd = -1;
    t->conn_fd   = -1;

    t->server_fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (t->server_fd < 0) { perror("socket"); free(t); return NULL; }

    struct sockaddr_rc addr = {
        .rc_family  = AF_BLUETOOTH,
        .rc_bdaddr  = *BDADDR_ANY,
        .rc_channel = channel
    };

    if (bind(t->server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(t->server_fd); free(t); return NULL;
    }

    listen(t->server_fd, 1);
    printf("[rfcomm] Waiting for connection on channel %d...\n", channel);

    struct sockaddr_rc client_addr = {0};
    socklen_t opt = sizeof(client_addr);
    t->conn_fd = accept(t->server_fd, (struct sockaddr*)&client_addr, &opt);
    if (t->conn_fd < 0) {
        perror("accept"); close(t->server_fd); free(t); return NULL;
    }

    char addr_str[18];
    ba2str(&client_addr.rc_bdaddr, addr_str);
    printf("[rfcomm] Connected from %s\n", addr_str);
    return t;
}

RFCOMMTransport* rfcomm_connect(const char *target_addr, uint8_t channel) {
    RFCOMMTransport *t = calloc(1, sizeof(*t));
    t->server_fd = -1;

    t->conn_fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (t->conn_fd < 0) { perror("socket"); free(t); return NULL; }

    struct sockaddr_rc addr = { .rc_family = AF_BLUETOOTH, .rc_channel = channel };
    str2ba(target_addr, &addr.rc_bdaddr);

    printf("[rfcomm] Connecting to %s channel %d...\n", target_addr, channel);
    if (connect(t->conn_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(t->conn_fd); free(t); return NULL;
    }
    printf("[rfcomm] Connected.\n");
    return t;
}

int rfcomm_send_packet(RFCOMMTransport *t, const AetherPacket *pkt) {
    int n = aether_packet_pack(pkt, t->tx_buf, sizeof(t->tx_buf));
    if (n < 0) return -1;

    /* Send in chunks respecting RFCOMM MTU */
    int sent = 0;
    while (sent < n) {
        int chunk = (n - sent < AETHER_RFCOMM_MTU) ? n - sent : AETHER_RFCOMM_MTU;
        int r = send(t->conn_fd, t->tx_buf + sent, chunk, 0);
        if (r < 0) { perror("send"); return -1; }
        sent += r;
    }
    return 0;
}

int rfcomm_recv_packet(RFCOMMTransport *t, AetherPacket *pkt_out) {
    /* Read header first */
    int received = 0;
    while (received < AETHER_HEADER_SIZE) {
        int r = recv(t->conn_fd, t->rx_buf + received,
                     AETHER_HEADER_SIZE - received, 0);
        if (r <= 0) return -1;
        received += r;
    }

    /* Parse payload size from header */
    AetherHeader hdr;
    memcpy(&hdr, t->rx_buf, AETHER_HEADER_SIZE);
    if (hdr.magic != AETHER_MAGIC) return -1;

    int total = AETHER_HEADER_SIZE + hdr.payload_size + 4;

    /* Read remaining bytes */
    while (received < total) {
        int r = recv(t->conn_fd, t->rx_buf + received, total - received, 0);
        if (r <= 0) return -1;
        received += r;
    }

    return aether_packet_unpack(t->rx_buf, total, pkt_out);
}

int rfcomm_get_fd(const RFCOMMTransport *t) { return t->conn_fd; }

void rfcomm_server_close(RFCOMMTransport *t) {
    if (t->conn_fd >= 0)   close(t->conn_fd);
    if (t->server_fd >= 0) close(t->server_fd);
    free(t);
}

void rfcomm_client_close(RFCOMMTransport *t) {
    if (t->conn_fd >= 0) close(t->conn_fd);
    free(t);
}
```

---

### Step 1.4 — Build a Raw Byte Ping Test

Before touching audio, verify the RFCOMM pipe works end-to-end.

Create `tools/rfcomm_ping.c`:

```c
/* Usage:
     Laptop B: ./rfcomm_ping server
     Laptop A: ./rfcomm_ping client AA:BB:CC:DD:EE:FF
*/
#include "aether_packet.h"
#include "transport_rfcomm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s server|client [addr]\n", argv[0]); return 1; }

    if (strcmp(argv[1], "server") == 0) {
        RFCOMMTransport *t = rfcomm_listen(RFCOMM_CHANNEL);
        if (!t) return 1;
        for (int i = 0; i < 10; i++) {
            AetherPacket pkt = {0};
            int r = rfcomm_recv_packet(t, &pkt);
            if (r < 0) { fprintf(stderr, "recv error\n"); break; }
            printf("[server] Got seq=%u payload='%.*s'\n",
                   pkt.hdr.sequence, pkt.hdr.payload_size, pkt.payload);
        }
        rfcomm_server_close(t);

    } else if (argc == 3) {
        RFCOMMTransport *t = rfcomm_connect(argv[2], RFCOMM_CHANNEL);
        if (!t) return 1;
        for (int i = 0; i < 10; i++) {
            AetherPacket pkt = {0};
            pkt.hdr.magic        = AETHER_MAGIC;
            pkt.hdr.sequence     = i;
            pkt.hdr.timestamp_us = aether_timestamp_us();
            pkt.hdr.mode         = AETHER_MODE_CTRL;
            char msg[32];
            snprintf(msg, sizeof(msg), "ping %d", i);
            memcpy(pkt.payload, msg, strlen(msg));
            pkt.hdr.payload_size = (uint16_t)strlen(msg);
            rfcomm_send_packet(t, &pkt);
            printf("[client] Sent seq=%d\n", i);
        }
        rfcomm_client_close(t);
    }
    return 0;
}
```

**On Laptop B:** `./rfcomm_ping server`  
**On Laptop A:** `./rfcomm_ping client AA:BB:CC:DD:EE:FF`

---

### Step 1.5 — Measure Raw Throughput and Latency

Create `tools/rfcomm_bench.c` — sends 1000 packets of 1400-byte payload, measures time:

```c
/* Laptop B: ./rfcomm_bench server
   Laptop A: ./rfcomm_bench client AA:BB:CC:DD:EE:FF */
#include "aether_packet.h"
#include "transport_rfcomm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NUM_PACKETS  1000
#define PAYLOAD_SIZE 1400   // ~1 frame of compressed audio

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;

    if (strcmp(argv[1], "server") == 0) {
        RFCOMMTransport *t = rfcomm_listen(RFCOMM_CHANNEL);
        if (!t) return 1;

        uint64_t start = aether_timestamp_us();
        int count = 0;
        while (count < NUM_PACKETS) {
            AetherPacket pkt = {0};
            if (rfcomm_recv_packet(t, &pkt) < 0) break;
            count++;
        }
        uint64_t elapsed_us = aether_timestamp_us() - start;
        double elapsed_s    = elapsed_us / 1e6;
        double kbps         = (NUM_PACKETS * PAYLOAD_SIZE * 8.0) / 1000.0 / elapsed_s;

        printf("[server] Received %d packets in %.3fs\n", count, elapsed_s);
        printf("[server] Throughput: %.1f kbps\n", kbps);
        rfcomm_server_close(t);

    } else if (argc == 3) {
        RFCOMMTransport *t = rfcomm_connect(argv[2], RFCOMM_CHANNEL);
        if (!t) return 1;

        uint64_t start = aether_timestamp_us();
        for (int i = 0; i < NUM_PACKETS; i++) {
            AetherPacket pkt = {0};
            pkt.hdr.magic        = AETHER_MAGIC;
            pkt.hdr.sequence     = i;
            pkt.hdr.timestamp_us = aether_timestamp_us();
            pkt.hdr.mode         = AETHER_MODE_NL;
            pkt.hdr.payload_size = PAYLOAD_SIZE;
            /* payload is zeroed — fine for benchmark */
            rfcomm_send_packet(t, &pkt);
        }
        uint64_t elapsed_us = aether_timestamp_us() - start;
        printf("[client] Sent %d packets in %.3fs (%.1f kbps)\n",
               NUM_PACKETS, elapsed_us / 1e6,
               (NUM_PACKETS * PAYLOAD_SIZE * 8.0) / 1000.0 / (elapsed_us / 1e6));
        rfcomm_client_close(t);
    }
    return 0;
}
```

**Target:** throughput should be ≥ 1,000 kbps. If it is below 600 kbps your Bluetooth chip may not support EDR — adjust target bitrate accordingly.

---

### Step 1.6 — Raw PCM Streaming (No Compression)

Stream actual audio samples over RFCOMM to prove end-to-end audio works before adding any codec.

Create `tools/raw_stream_sender.c` — reads stdin as raw PCM, sends over RFCOMM:

```c
/* Laptop B: ./raw_stream_receiver
   Laptop A: sox input.flac -t raw -r 96000 -b 16 -c 2 -e signed - | ./raw_stream_sender AA:BB:CC:DD:EE:FF */

#define FRAME_SAMPLES 2048
#define FRAME_BYTES   (FRAME_SAMPLES * 2 * 2)  // 16-bit stereo

#include "aether_packet.h"
#include "transport_rfcomm.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc != 2) { fprintf(stderr, "Usage: %s BT_ADDR\n", argv[0]); return 1; }

    RFCOMMTransport *t = rfcomm_connect(argv[1], RFCOMM_CHANNEL);
    if (!t) return 1;

    uint8_t pcm_buf[FRAME_BYTES];
    uint32_t seq = 0;

    while (fread(pcm_buf, 1, FRAME_BYTES, stdin) == FRAME_BYTES) {
        AetherPacket pkt = {0};
        pkt.hdr.magic        = AETHER_MAGIC;
        pkt.hdr.sequence     = seq++;
        pkt.hdr.timestamp_us = aether_timestamp_us();
        pkt.hdr.mode         = AETHER_MODE_NL;   // raw PCM for now
        pkt.hdr.sample_rate  = AETHER_RATE_96000;
        pkt.hdr.bit_depth    = 16;
        pkt.hdr.channels     = 2;
        pkt.hdr.payload_size = FRAME_BYTES;
        memcpy(pkt.payload, pcm_buf, FRAME_BYTES);
        if (rfcomm_send_packet(t, &pkt) < 0) break;
    }

    rfcomm_client_close(t);
    return 0;
}
```

Create `tools/raw_stream_receiver.c` — receives, writes to stdout for playback:

```c
/* Laptop B: ./raw_stream_receiver | aplay -r 96000 -f S16_LE -c 2 */
#include "aether_packet.h"
#include "transport_rfcomm.h"
#include <stdio.h>

int main(void) {
    RFCOMMTransport *t = rfcomm_listen(RFCOMM_CHANNEL);
    if (!t) return 1;

    AetherPacket pkt = {0};
    while (rfcomm_recv_packet(t, &pkt) == 0) {
        fwrite(pkt.payload, 1, pkt.hdr.payload_size, stdout);
    }

    rfcomm_server_close(t);
    return 0;
}
```

**Test:**
```bash
# Laptop B
./raw_stream_receiver | aplay -r 96000 -f S16_LE -c 2

# Laptop A (requires a test WAV/FLAC file)
sox test.flac -t raw -r 96000 -b 16 -c 2 -e signed - | ./raw_stream_sender AA:BB:CC:DD:EE:FF
```

You should hear audio on Laptop B's headphones. It may drop packets at high bitrate — that's expected and what the codec is for.

---

### ✅ Phase 1 Checkpoint

- [x] `test_packet` passes (pack/unpack/CRC verified) — `ctest`: 100% tests passed, 0 failed
- [x] `rfcomm_ping` works: Laptop A (`C0:35:32:24:8E:D0`) sent, Laptop B
      (`FC:B0:DE:80:0C:B4`, `samiran-Inspiron-15-3525`) received all 10 pings —
      verified over paired/bonded RFCOMM channel 1
- [ ] `rfcomm_bench` shows ≥ 600 kbps throughput (record exact number) — pending, now that laptops are paired
- [ ] `raw_stream` plays audible audio on Laptop B (may drop at 96kHz, ok) — pending, now that laptops are paired
- [ ] Measured throughput recorded: _________ kbps

> **Note:** `AETHER_HEADER_SIZE` was corrected from **24** to **20** bytes. The
> field layout (magic 4 + sequence 4 + timestamp 4 + mode/rate/depth/channels 4
> + payload_size 2 + header_crc16 2 = 20) and the packet diagram in HLD §8.1
> ("payload begins at byte 20") both point to 20; the "24 bytes" total quoted
> in HLD §8.1's prose was inconsistent with its own diagram. A
> `_Static_assert(sizeof(AetherHeader) == AETHER_HEADER_SIZE)` was added to
> `aether_packet.h` to catch any future drift at compile time.

---

## Phase 2 — Near-Lossless Codec (LPC + Rice)

> **Goal:** Compress PCM audio using Linear Predictive Coding and Rice entropy coding. Verify lossless round-trip — decoded audio must be bit-for-bit identical to the original.

---

### Step 2.1 — Implement Fixed-Point Autocorrelation

Create `include/codec_lpc.h`:

```c
#ifndef CODEC_LPC_H
#define CODEC_LPC_H

#include <stdint.h>

#define LPC_MAX_ORDER    32
#define LPC_FRAME_SIZE   2048
#define LPC_COEFF_SHIFT  15   // Q15 fixed-point for LPC coefficients

/* LPC encoder state for one frame */
typedef struct {
    int32_t  coeffs[LPC_MAX_ORDER];  // Q15 fixed-point predictor coefficients
    int      order;                  // chosen LPC order (8, 16, or 32)
    int32_t  warmup[LPC_MAX_ORDER];  // first `order` unencoded samples
} LPCFrameHeader;

/* Encode one frame of 24-bit PCM samples.
   samples:     input, LPC_FRAME_SIZE int32_t values (24-bit range)
   hdr_out:     output LPC frame header (coefficients + order + warmup)
   residuals:   output array, LPC_FRAME_SIZE int32_t values
   Returns 0 on success. */
int  lpc_encode_frame(const int32_t *samples, int n,
                      LPCFrameHeader *hdr_out, int32_t *residuals);

/* Decode one frame.
   hdr:         LPC frame header from encoder
   residuals:   Rice-decoded residuals
   samples_out: output, n int32_t values
   Returns 0 on success. */
int  lpc_decode_frame(const LPCFrameHeader *hdr, const int32_t *residuals,
                      int n, int32_t *samples_out);

/* Select best LPC order for this frame's energy profile */
int  lpc_select_order(const int32_t *samples, int n);

/* Rice entropy coding */
int  rice_encode(const int32_t *residuals, int n, int rice_param,
                 uint8_t *bitstream_out, int max_bytes);
int  rice_decode(const uint8_t *bitstream, int byte_len, int rice_param,
                 int n, int32_t *residuals_out);
int  rice_select_param(const int32_t *residuals, int n);

#endif
```

Create `src/encoder/codec_lpc_enc.c`:

```c
#include "codec_lpc.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

int lpc_select_order(const int32_t *samples, int n) {
    /* Estimate signal complexity via zero-crossing rate */
    int zcr = 0;
    for (int i = 1; i < n; i++)
        if ((samples[i] >= 0) != (samples[i-1] >= 0)) zcr++;

    double zcr_rate = (double)zcr / n;
    if (zcr_rate > 0.3)  return 32;   // high frequency content
    if (zcr_rate > 0.15) return 16;
    return 8;                           // smooth / bass-heavy
}

int lpc_encode_frame(const int32_t *samples, int n,
                     LPCFrameHeader *hdr_out, int32_t *residuals) {
    int order = lpc_select_order(samples, n);
    hdr_out->order = order;

    /* Store warm-up samples verbatim (needed for decoder) */
    for (int i = 0; i < order; i++)
        hdr_out->warmup[i] = samples[i];

    /* Step 1: Compute autocorrelation R[0..order] */
    double R[LPC_MAX_ORDER + 1] = {0};
    for (int lag = 0; lag <= order; lag++) {
        double sum = 0;
        for (int i = lag; i < n; i++)
            sum += (double)samples[i] * samples[i - lag];
        R[lag] = sum;
    }

    if (R[0] == 0.0) {
        /* Silence frame — all residuals are zero */
        memset(residuals, 0, n * sizeof(int32_t));
        memset(hdr_out->coeffs, 0, order * sizeof(int32_t));
        return 0;
    }

    /* Step 2: Levinson-Durbin recursion → LPC coefficients */
    double a[LPC_MAX_ORDER + 1] = {0};  // double precision during solve
    double err = R[0];

    for (int i = 1; i <= order; i++) {
        double lambda = 0;
        for (int j = 1; j <= i - 1; j++)
            lambda += a[j] * R[i - j];
        lambda = (R[i] - lambda) / err;

        a[i] = lambda;
        for (int j = 1; j <= i / 2; j++) {
            double tmp  = a[j] - lambda * a[i - j];
            a[i - j]   = a[i - j] - lambda * a[j];
            a[j]        = tmp;
        }
        err *= 1.0 - lambda * lambda;
        if (err <= 0.0) { order = i - 1; hdr_out->order = order; break; }
    }

    /* Step 3: Quantize coefficients to Q15 fixed-point */
    for (int i = 0; i < order; i++)
        hdr_out->coeffs[i] = (int32_t)(a[i + 1] * (1 << LPC_COEFF_SHIFT));

    /* Step 4: Compute residuals  e[n] = x[n] - sum(a[k]*x[n-k]) */
    for (int i = 0; i < order; i++)
        residuals[i] = 0;  // warm-up residuals not used

    for (int i = order; i < n; i++) {
        int64_t predicted = 0;
        for (int k = 0; k < order; k++)
            predicted += (int64_t)hdr_out->coeffs[k] * samples[i - k - 1];
        predicted >>= LPC_COEFF_SHIFT;
        residuals[i] = samples[i] - (int32_t)predicted;
    }

    return 0;
}
```

Create `src/decoder/codec_lpc_dec.c`:

```c
#include "codec_lpc.h"
#include <string.h>

int lpc_decode_frame(const LPCFrameHeader *hdr, const int32_t *residuals,
                     int n, int32_t *out) {
    int order = hdr->order;

    /* Restore warm-up samples */
    for (int i = 0; i < order; i++)
        out[i] = hdr->warmup[i];

    /* LPC synthesis: x[n] = e[n] + sum(a[k]*x[n-k]) */
    for (int i = order; i < n; i++) {
        int64_t predicted = 0;
        for (int k = 0; k < order; k++)
            predicted += (int64_t)hdr->coeffs[k] * out[i - k - 1];
        predicted >>= LPC_COEFF_SHIFT;
        out[i] = residuals[i] + (int32_t)predicted;
    }

    return 0;
}
```

---

### Step 2.2 — Implement Rice Entropy Coding

Append to `src/encoder/codec_lpc_enc.c` (or separate file):

```c
/* Rice coding encodes signed integers by mapping to unsigned:
   n >= 0 → 2n,  n < 0 → 2|n|-1  (zigzag encoding) */

static inline uint32_t zigzag_encode(int32_t n) {
    return (n >= 0) ? (uint32_t)(n * 2) : (uint32_t)(-n * 2 - 1);
}

static inline int32_t zigzag_decode(uint32_t u) {
    return (u & 1) ? -(int32_t)((u + 1) >> 1) : (int32_t)(u >> 1);
}

int rice_select_param(const int32_t *residuals, int n) {
    /* Optimal Rice param k ≈ log2(mean(|residuals|)) - 1 */
    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += abs(residuals[i]);
    double mean = sum / n;
    if (mean < 1) return 0;
    int k = (int)(log2(mean) - 0.5);
    if (k < 0)  k = 0;
    if (k > 15) k = 15;
    return k;
}

/* Simple bit-packing writer */
typedef struct { uint8_t *buf; int cap; int byte_pos; int bit_pos; } BitWriter;

static void bw_init(BitWriter *bw, uint8_t *buf, int cap) {
    bw->buf = buf; bw->cap = cap; bw->byte_pos = 0; bw->bit_pos = 0;
}

static int bw_write_bit(BitWriter *bw, int bit) {
    if (bw->byte_pos >= bw->cap) return -1;
    if (bit) bw->buf[bw->byte_pos] |= (1 << (7 - bw->bit_pos));
    if (++bw->bit_pos == 8) { bw->bit_pos = 0; bw->byte_pos++; }
    return 0;
}

static int bw_flush(BitWriter *bw) {
    if (bw->bit_pos > 0) bw->byte_pos++;
    return bw->byte_pos;
}

int rice_encode(const int32_t *residuals, int n, int k,
                uint8_t *out, int max_bytes) {
    BitWriter bw;
    bw_init(&bw, out, max_bytes);
    memset(out, 0, max_bytes);

    for (int i = 0; i < n; i++) {
        uint32_t u = zigzag_encode(residuals[i]);
        uint32_t q = u >> k;          // quotient (unary coded)
        uint32_t r = u & ((1 << k) - 1); // remainder (binary coded, k bits)

        /* Write q ones then a zero (unary coding) */
        for (uint32_t j = 0; j < q; j++)
            if (bw_write_bit(&bw, 1) < 0) return -1;
        if (bw_write_bit(&bw, 0) < 0) return -1;

        /* Write k-bit remainder MSB first */
        for (int b = k - 1; b >= 0; b--)
            if (bw_write_bit(&bw, (r >> b) & 1) < 0) return -1;
    }

    return bw_flush(&bw);
}

/* Bit reader for decode */
typedef struct { const uint8_t *buf; int len; int byte_pos; int bit_pos; } BitReader;

static void br_init(BitReader *br, const uint8_t *buf, int len) {
    br->buf = buf; br->len = len; br->byte_pos = 0; br->bit_pos = 0;
}

static int br_read_bit(BitReader *br) {
    if (br->byte_pos >= br->len) return -1;
    int bit = (br->buf[br->byte_pos] >> (7 - br->bit_pos)) & 1;
    if (++br->bit_pos == 8) { br->bit_pos = 0; br->byte_pos++; }
    return bit;
}

int rice_decode(const uint8_t *bitstream, int byte_len, int k,
                int n, int32_t *out) {
    BitReader br;
    br_init(&br, bitstream, byte_len);

    for (int i = 0; i < n; i++) {
        /* Read unary quotient */
        uint32_t q = 0;
        int bit;
        while ((bit = br_read_bit(&br)) == 1) q++;
        if (bit < 0) return -1;

        /* Read k-bit remainder */
        uint32_t r = 0;
        for (int b = k - 1; b >= 0; b--) {
            bit = br_read_bit(&br);
            if (bit < 0) return -1;
            r |= (uint32_t)bit << b;
        }

        out[i] = zigzag_decode((q << k) | r);
    }
    return 0;
}
```

---

### Step 2.3 — Lossless Round-Trip Test

This is the most important test in the entire project. The decoded output must match the input **exactly**.

Create `tests/test_lpc.c`:

```c
#include "codec_lpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

int main(void) {
    srand(42);

    /* Test 1: Synthetic sine wave */
    int32_t original[LPC_FRAME_SIZE];
    for (int i = 0; i < LPC_FRAME_SIZE; i++)
        original[i] = (int32_t)(8000000.0 * sin(2.0 * 3.14159 * 440.0 * i / 96000.0));

    LPCFrameHeader hdr = {0};
    int32_t residuals[LPC_FRAME_SIZE] = {0};
    int r = lpc_encode_frame(original, LPC_FRAME_SIZE, &hdr, residuals);
    assert(r == 0);

    /* Rice code the residuals */
    int k = rice_select_param(residuals + hdr.order, LPC_FRAME_SIZE - hdr.order);
    uint8_t bitstream[LPC_FRAME_SIZE * 6];  // generous buffer
    int bytes = rice_encode(residuals + hdr.order, LPC_FRAME_SIZE - hdr.order,
                            k, bitstream, sizeof(bitstream));
    assert(bytes > 0);

    /* Decode residuals */
    int32_t dec_residuals[LPC_FRAME_SIZE] = {0};
    r = rice_decode(bitstream, bytes, k, LPC_FRAME_SIZE - hdr.order,
                    dec_residuals + hdr.order);
    assert(r == 0);

    /* LPC synthesis */
    int32_t decoded[LPC_FRAME_SIZE] = {0};
    r = lpc_decode_frame(&hdr, dec_residuals, LPC_FRAME_SIZE, decoded);
    assert(r == 0);

    /* Verify bit-perfect reconstruction */
    int mismatches = 0;
    for (int i = 0; i < LPC_FRAME_SIZE; i++) {
        if (original[i] != decoded[i]) {
            fprintf(stderr, "MISMATCH at i=%d: orig=%d decoded=%d\n",
                    i, original[i], decoded[i]);
            mismatches++;
            if (mismatches > 10) break;
        }
    }
    assert(mismatches == 0);

    /* Compression ratio */
    int raw_bytes = LPC_FRAME_SIZE * 3;  // 24-bit = 3 bytes/sample
    printf("✓ LPC round-trip: LOSSLESS (0 mismatches)\n");
    printf("  Raw: %d bytes | Compressed: %d bytes | Ratio: %.2fx\n",
           raw_bytes, bytes,
           (double)raw_bytes / bytes);

    /* Test 2: Random noise (hardest case for LPC) */
    for (int i = 0; i < LPC_FRAME_SIZE; i++)
        original[i] = (rand() % (1 << 23)) - (1 << 22);

    memset(&hdr, 0, sizeof(hdr));
    memset(residuals, 0, sizeof(residuals));
    lpc_encode_frame(original, LPC_FRAME_SIZE, &hdr, residuals);
    k = rice_select_param(residuals + hdr.order, LPC_FRAME_SIZE - hdr.order);
    bytes = rice_encode(residuals + hdr.order, LPC_FRAME_SIZE - hdr.order,
                        k, bitstream, sizeof(bitstream));
    memset(dec_residuals, 0, sizeof(dec_residuals));
    rice_decode(bitstream, bytes, k, LPC_FRAME_SIZE - hdr.order,
                dec_residuals + hdr.order);
    memset(decoded, 0, sizeof(decoded));
    lpc_decode_frame(&hdr, dec_residuals, LPC_FRAME_SIZE, decoded);

    mismatches = 0;
    for (int i = 0; i < LPC_FRAME_SIZE; i++)
        if (original[i] != decoded[i]) mismatches++;
    assert(mismatches == 0);
    printf("✓ LPC round-trip (noise): LOSSLESS\n");
    printf("  Raw: %d bytes | Compressed: %d bytes | Ratio: %.2fx\n",
           LPC_FRAME_SIZE * 3, bytes,
           (double)(LPC_FRAME_SIZE * 3) / bytes);

    return 0;
}
```

**This test must pass before proceeding. Zero mismatches is the only acceptable result.**

---

### Step 2.4 — Integrate NL Codec with Transport

Create `src/encoder/aether_encoder.c` (NL path only for now):

```c
#include "aether_encoder.h"
#include "codec_lpc.h"
#include "aether_packet.h"
#include <stdlib.h>
#include <string.h>

struct AetherEncoder {
    int mode;
    int sample_rate;
    int bit_depth;
    int channels;
    uint32_t sequence;
};

AetherEncoder* aether_encoder_create(int mode, int sample_rate,
                                      int bit_depth, int channels) {
    AetherEncoder *enc = calloc(1, sizeof(*enc));
    enc->mode        = mode;
    enc->sample_rate = sample_rate;
    enc->bit_depth   = bit_depth;
    enc->channels    = channels;
    enc->sequence    = 0;
    return enc;
}

/* Encode one stereo frame (interleaved L,R samples) */
int aether_encoder_encode(AetherEncoder *enc, const int32_t *pcm,
                           int frame_samples, AetherPacket *pkt_out) {
    memset(pkt_out, 0, sizeof(*pkt_out));
    pkt_out->hdr.magic        = AETHER_MAGIC;
    pkt_out->hdr.sequence     = enc->sequence++;
    pkt_out->hdr.timestamp_us = aether_timestamp_us();
    pkt_out->hdr.mode         = enc->mode;
    pkt_out->hdr.sample_rate  = AETHER_RATE_96000;
    pkt_out->hdr.bit_depth    = (uint8_t)enc->bit_depth;
    pkt_out->hdr.channels     = (uint8_t)enc->channels;

    /* De-interleave L and R channels */
    int32_t left[LPC_FRAME_SIZE], right[LPC_FRAME_SIZE];
    for (int i = 0; i < frame_samples; i++) {
        left[i]  = pcm[i * 2];
        right[i] = pcm[i * 2 + 1];
    }

    uint8_t *p = pkt_out->payload;
    int remaining = sizeof(pkt_out->payload);

    /* Encode left channel */
    LPCFrameHeader hdr_l = {0};
    int32_t res_l[LPC_FRAME_SIZE] = {0};
    lpc_encode_frame(left, frame_samples, &hdr_l, res_l);

    int k_l = rice_select_param(res_l + hdr_l.order, frame_samples - hdr_l.order);

    /* Write header: order (1 byte) + rice_param (1 byte) + warmup samples */
    p[0] = (uint8_t)hdr_l.order;
    p[1] = (uint8_t)k_l;
    p += 2; remaining -= 2;
    memcpy(p, hdr_l.coeffs, hdr_l.order * sizeof(int32_t));
    p += hdr_l.order * sizeof(int32_t); remaining -= hdr_l.order * sizeof(int32_t);
    memcpy(p, hdr_l.warmup, hdr_l.order * sizeof(int32_t));
    p += hdr_l.order * sizeof(int32_t); remaining -= hdr_l.order * sizeof(int32_t);

    /* Rice-encode residuals */
    int bytes_l = rice_encode(res_l + hdr_l.order, frame_samples - hdr_l.order,
                               k_l, p + 2, remaining - 2);
    if (bytes_l < 0) return -1;
    /* Store residual byte count (2 bytes) */
    uint16_t bl = (uint16_t)bytes_l;
    memcpy(p, &bl, 2); p += 2 + bytes_l; remaining -= 2 + bytes_l;

    /* Repeat for right channel */
    LPCFrameHeader hdr_r = {0};
    int32_t res_r[LPC_FRAME_SIZE] = {0};
    lpc_encode_frame(right, frame_samples, &hdr_r, res_r);
    int k_r = rice_select_param(res_r + hdr_r.order, frame_samples - hdr_r.order);
    p[0] = (uint8_t)hdr_r.order;
    p[1] = (uint8_t)k_r;
    p += 2; remaining -= 2;
    memcpy(p, hdr_r.coeffs, hdr_r.order * sizeof(int32_t));
    p += hdr_r.order * sizeof(int32_t); remaining -= hdr_r.order * sizeof(int32_t);
    memcpy(p, hdr_r.warmup, hdr_r.order * sizeof(int32_t));
    p += hdr_r.order * sizeof(int32_t); remaining -= hdr_r.order * sizeof(int32_t);
    int bytes_r = rice_encode(res_r + hdr_r.order, frame_samples - hdr_r.order,
                               k_r, p + 2, remaining - 2);
    if (bytes_r < 0) return -1;
    uint16_t br16 = (uint16_t)bytes_r;
    memcpy(p, &br16, 2); p += 2 + bytes_r;

    pkt_out->hdr.payload_size = (uint16_t)(p - pkt_out->payload);
    return 0;
}

void aether_encoder_set_mode(AetherEncoder *enc, int mode) { enc->mode = mode; }
void aether_encoder_destroy(AetherEncoder *enc) { free(enc); }
```

---

### Step 2.5 — File-to-File Compression Test

Before streaming over Bluetooth, test encoder → decoder on a real audio file:

```bash
# Encode a FLAC file to AetherCodec bitstream
./tools/aether_encode input.flac output.aether

# Decode it back
./tools/aether_decode output.aether decoded.wav

# Compare with sox
sox input.flac original.wav
diff <(xxd original.wav) <(xxd decoded.wav)
# Must be identical (after accounting for WAV header differences — compare raw PCM)

# Or use: sox original.wav decoded.wav -n remix - stats
# All values should be 0 (difference signal is silence)
```

Create simple encode/decode tools in `tools/` that call the encoder and write raw bitstream to file.

---

### ✅ Phase 2 Checkpoint

- [x] `test_lpc` passes: 0 mismatches on sine wave, noise, **and** silence (bit-perfect)
- [x] `test_codec_nl` passes: full encoder→pack→CRC→unpack→decoder stereo round-trip is bit-perfect (integration test)
- [ ] Compression ratio ≥ 2x on music content (record: ____x) — pending real music file; synthetic tone gives ~1.4x, silence 24x, noise 1.0x (incompressible, as expected)
- [ ] File-to-file round-trip: decoded WAV is bit-perfect match to original — WAV I/O tools not built yet; codec-level round-trip proven by `test_codec_nl`
- [ ] NL-encoded packets stream over RFCOMM and decode on Laptop B — needs second paired laptop
- [ ] Compressed audio plays correctly on Laptop B headphones — needs second paired laptop

> **Rice parameter range widened 0–15 → 0–30** (`LPC_RICE_MAX_PARAM` in
> `codec_lpc.h`). Capping k at 15 makes the unary quotient explode on
> high-entropy frames (noise), overflowing the output buffer and breaking the
> lossless guarantee. k is stored as a full byte on the wire, so the wider
> ceiling is free and keeps worst-case output bounded. Verified by the noise
> case in `test_lpc` (round-trips losslessly at 1.0x instead of failing).

---

## Phase 3 — Perceptual HQ Codec (MDCT + Psychoacoustics)

> **Goal:** Implement a lossy but perceptually transparent codec using MDCT and psychoacoustic masking. Target: indistinguishable from the original at 900–1,100 kbps.

---

### Step 3.1 — Implement KBD Window

The Kaiser-Bessel Derived window is used before MDCT to reduce spectral leakage:

```c
// src/encoder/codec_mdct_enc.c

#include "codec_mdct.h"
#include <math.h>
#include <string.h>

#define MDCT_SIZE     1024   // one channel, one frame
#define MDCT_OVERLAP  512    // 50% overlap

static float kbd_window[MDCT_SIZE * 2];
static int   kbd_window_init = 0;

static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k <= 30; k++) {
        term *= (x / 2.0) / k;
        term *= term;  // wait — this is wrong; see below
        // Correct: term = term_k = (x/2)^(2k) / (k!)^2
        sum += term;
    }
    return sum;
}

void mdct_init_window(void) {
    if (kbd_window_init) return;
    int N = MDCT_SIZE * 2;
    double alpha = 4.0 * M_PI;  // Kaiser beta parameter

    /* Kaiser window */
    double kaiser[MDCT_SIZE * 2];
    double denom = 1.0; /* bessel_i0(alpha) */
    /* Compute I0(alpha) */
    denom = 1.0;
    double term = 1.0;
    for (int k = 1; k <= 30; k++) {
        term *= (alpha / 2.0) / k;
        denom += term * term;
    }
    for (int n = 0; n < N; n++) {
        double r = 2.0 * n / (N - 1) - 1.0;
        double arg = alpha * sqrt(1.0 - r * r);
        double val = 1.0;
        term = 1.0;
        for (int k = 1; k <= 30; k++) {
            term *= (arg / 2.0) / k;
            val += term * term;
        }
        kaiser[n] = val / denom;
    }

    /* KBD: cumulative sum then sqrt */
    double cumsum[MDCT_SIZE * 2 + 1] = {0};
    for (int n = 0; n < N; n++)
        cumsum[n + 1] = cumsum[n] + kaiser[n];
    double total = cumsum[N];
    for (int n = 0; n < N; n++)
        kbd_window[n] = (float)sqrt(cumsum[n + 1] / total);

    kbd_window_init = 1;
}
```

---

### Step 3.2 — Implement MDCT via FFTW3

```c
#include <fftw3.h>

/* MDCT of N samples → N/2 frequency coefficients */
void mdct_transform(const float *windowed, int N, float *out) {
    int M = N / 2;
    /* MDCT formula: X[k] = sum_{n=0}^{N-1} x[n] * cos(pi/N * (n+0.5+M/2) * (k+0.5)) */
    /* Implement via FFT trick: pre-rotate, FFT, post-rotate */

    float *pre = (float*)fftwf_malloc(M * 2 * sizeof(float));
    fftwf_complex *fft_out = (fftwf_complex*)fftwf_malloc((M + 1) * sizeof(fftwf_complex));

    /* Pre-rotation */
    for (int n = 0; n < M; n++) {
        float angle = (float)(M_PI / (2 * M) * (2 * n + 1));
        pre[2 * n]     = windowed[n]     * cosf(angle) + windowed[N - 1 - n] * sinf(angle);
        pre[2 * n + 1] = windowed[M + n] * cosf(angle) - windowed[M - 1 - n] * sinf(angle);
    }

    fftwf_plan plan = fftwf_plan_dft_r2c_1d(M * 2, pre, fft_out, FFTW_ESTIMATE);
    fftwf_execute(plan);

    /* Post-rotation */
    for (int k = 0; k < M; k++) {
        float angle = (float)(M_PI / (2 * M) * (k + 0.5));
        out[k] = 2.0f * (fftwf_creal(fft_out[k]) * cosf(angle) +
                         fftwf_cimag(fft_out[k]) * sinf(angle));
    }

    fftwf_destroy_plan(plan);
    fftwf_free(pre);
    fftwf_free(fft_out);
}
```

---

### Step 3.3 — Bark-Scale Band Mapping

```c
/* Map MDCT bin index to Bark band (0–63) at 96kHz */
/* Bark scale: z = 13*atan(0.76*f/1000) + 3.5*atan((f/7500)^2) */

#define BARK_BANDS 64

static int bark_band_for_bin[MDCT_SIZE / 2];  // 512 bins → band index
static int band_start[BARK_BANDS + 1];         // first bin of each band

void mdct_init_bark_table(int sample_rate) {
    int M = MDCT_SIZE / 2;
    /* Assign each bin to a Bark band */
    for (int k = 0; k < M; k++) {
        double f = (double)k * sample_rate / MDCT_SIZE;
        double bark = 13.0 * atan(0.76 * f / 1000.0)
                    + 3.5 * atan((f / 7500.0) * (f / 7500.0));
        int band = (int)(bark / 24.0 * BARK_BANDS);  // scale to 64 bands
        if (band >= BARK_BANDS) band = BARK_BANDS - 1;
        bark_band_for_bin[k] = band;
    }

    /* Find start bin of each band */
    int current_band = -1;
    for (int k = 0; k < M; k++) {
        if (bark_band_for_bin[k] != current_band) {
            current_band = bark_band_for_bin[k];
            band_start[current_band] = k;
        }
    }
    band_start[BARK_BANDS] = M;
}
```

---

### Step 3.4 — Psychoacoustic Masking Threshold

```c
/* Compute masking threshold per Bark band.
   mdct_coeffs: 512 float frequency coefficients
   mask_out:    64 float masking thresholds (energy units)  */
void compute_masking_threshold(const float *mdct_coeffs, float *mask_out) {
    float band_energy[BARK_BANDS] = {0};

    /* Compute energy per band */
    for (int k = 0; k < MDCT_SIZE / 2; k++) {
        int b = bark_band_for_bin[k];
        band_energy[b] += mdct_coeffs[k] * mdct_coeffs[k];
    }

    /* Spreading function: masker at band i spreads to band j
       with attenuation based on distance |i-j| in Bark units */
    for (int j = 0; j < BARK_BANDS; j++) {
        float spread = 0;
        for (int i = 0; i < BARK_BANDS; i++) {
            float dz = (float)(j - i);
            float sf;
            if (dz >= 0)
                sf = powf(10.0f, (-2.5f * dz) / 10.0f);  // upward masking
            else
                sf = powf(10.0f, (1.5f * dz) / 10.0f);   // downward masking (steeper)
            spread += band_energy[i] * sf;
        }
        /* Threshold = spread energy * masking ratio (typically -12 dB below masker) */
        mask_out[j] = spread * 0.0631f;  // 10^(-12/10) ≈ 0.0631
    }

    /* Absolute hearing threshold (minimum audible field in quiet) */
    for (int j = 0; j < BARK_BANDS; j++) {
        float f = (float)(band_start[j]) * 96000.0f / MDCT_SIZE;
        float ath = powf(10.0f,
            (3.64f * powf(f / 1000.0f, -0.8f)
           - 6.5f  * expf(-0.6f * (f / 1000.0f - 3.3f) * (f / 1000.0f - 3.3f))
           + 1e-3f * powf(f / 1000.0f, 4.0f)) / 10.0f);
        if (ath > mask_out[j]) mask_out[j] = ath;
    }
}
```

---

### Step 3.5 — Bit Allocation and Quantization

```c
/* Allocate bits to each band so quantization noise falls below masking threshold.
   Returns: bits_per_band[BARK_BANDS] */
void allocate_bits(const float *band_energy, const float *mask,
                   int total_bits, int *bits_per_band) {
    /* Simplified uniform SNR allocation:
       more bits where SNR needed = energy/mask is largest */
    float snr_need[BARK_BANDS];
    float snr_total = 0;
    for (int b = 0; b < BARK_BANDS; b++) {
        snr_need[b] = (mask[b] > 0) ? band_energy[b] / mask[b] : 0;
        snr_total += snr_need[b];
    }

    for (int b = 0; b < BARK_BANDS; b++) {
        if (snr_total > 0)
            bits_per_band[b] = (int)(total_bits * snr_need[b] / snr_total);
        else
            bits_per_band[b] = total_bits / BARK_BANDS;
        if (bits_per_band[b] < 0)  bits_per_band[b] = 0;
        if (bits_per_band[b] > 15) bits_per_band[b] = 15;
    }
}

/* Quantize MDCT coefficients per band */
void quantize_band(float *coeffs, int start, int end,
                   float band_energy, int bits, float *quant_step_out) {
    if (bits == 0 || band_energy == 0) {
        for (int k = start; k < end; k++) coeffs[k] = 0;
        *quant_step_out = 0;
        return;
    }
    float levels = (float)(1 << bits);
    float peak   = sqrtf(band_energy / (end - start)) * 4.0f;
    float step   = peak / levels;
    *quant_step_out = step;
    for (int k = start; k < end; k++) {
        int q = (int)(coeffs[k] / step + 0.5f);
        coeffs[k] = (float)q * step;   // dequantized immediately for simplicity
    }
}
```

---

### Step 3.6 — Perceptual Quality Test

After implementing the HQ encoder/decoder:

```bash
# Encode and decode a music file
./tools/aether_encode --mode hq input.flac output_hq.aether
./tools/aether_decode output_hq.aether decoded_hq.wav

# Spectrum comparison with sox
sox input.flac -n spectrogram -o original_spec.png
sox decoded_hq.wav -n spectrogram -o decoded_spec.png

# Difference signal analysis
sox -m input.flac decoded_hq.wav -n remix - stats
# "DC offset" should be near 0, "RMS lev" of difference should be very low
```

---

### ✅ Phase 3 Checkpoint

- [x] MDCT round-trip test: error **4.2e-07** (< 1e-5) — `test_mdct`, via windowed overlap-add (see note below)
- [x] KBD window satisfies Princen-Bradley to 6.1e-08 (prerequisite for the above)
- [x] HQ encode→decode works end-to-end with CRC-checked packets — `test_codec_hq`: audible-band SNR ~27.5 dB, broadband bitrate **1048 kbps** (inside the 900–1,100 target)
- [ ] HQ encoded file sounds transparent at 900 kbps+ on headphones — needs listening test on real music
- [ ] Spectrum comparison shows no audible artifacts on music content — needs WAV file tools
- [ ] Both NL and HQ modes stream correctly over RFCOMM — needs second paired laptop

> **Corrections made in Phase 3** (the doc's Step 3.1–3.5 code does not run as written):
>
> - **`bessel_i0` was wrong** — the doc squares `term` in place each iteration
>   (it even flags this inline: *"wait — this is wrong"*), computing a
>   doubly-squared series. Correct recurrence: `term *= (x/2)/k; sum += term*term`.
> - **The MDCT sketch was not a valid MDCT** — it used a real-to-complex FFTW plan
>   with `fftwf_creal`/`fftwf_cimag`, which are not FFTW APIs, and its
>   pre/post-rotation did not correspond to the MDCT. Replaced with the standard
>   **fold + DCT-IV** formulation (`FFTW_REDFT11`), which is exact and O(N log N).
> - **Spreading function direction was inverted** — the doc uses −2.5 dB upward
>   and 1.5 dB downward, making the downward skirt *shallower*. Measured
>   psychoacoustics has downward steeper; now −2.5 up / −6.0 down.
> - **Bark scaling saturated** — dividing by a fixed 24 Bark dumps ~2/3 of the
>   96 kHz spectrum into the last band. Now normalised by the Bark value at
>   Nyquist so 64 bands span the represented range.
> - **ATH overflowed** — `1e-3·f_kHz⁴` reaches ~5300 dB at 48 kHz, overflowing
>   `powf`. Clamped, and pinned to digital full scale (2²³ ≈ 96 dB SPL).
> - **SMR raised 12 dB → 30 dB** (`MDCT_SMR_DB`). At 12 dB the codec produced only
>   ~13 dB SNR at ~490 kbps — under half the HQ budget. 30 dB lands in target range.
> - **Entropy stage uses Rice, not Huffman.** The HLD calls for "fixed Huffman
>   tables v1.0" but never specifies them; untrained tables would be arbitrary.
>   The already-proven Rice coder is adaptive per frame and needs no shipped tables.
>
> **On the checkpoint wording:** `inverse_mdct(mdct(x)) ≈ x` is not achievable for
> a *single* MDCT frame — the IMDCT of one frame is time-domain aliased by
> construction. Perfect reconstruction is a property of windowed overlap-add
> across successive frames (TDAC); that is what `test_mdct` verifies.

---

## Phase 4 — OS Integration (PipeWire)

> **Goal:** AetherCodec appears as a system audio device on both laptops. Any app on Laptop A can select it as audio output and hear the result on Laptop B's headphones.

---

### Step 4.1 — PipeWire Virtual Sink on Laptop B

Create `src/os/pw_sink.c`:

```c
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include "transport_rfcomm.h"
#include "aether_decoder.h"

#define SAMPLE_RATE  96000
#define CHANNELS     2
#define FRAME_SIZE   2048

static void on_process(void *userdata) {
    /* PipeWire calls this when audio data is available for playback */
    struct pw_stream *stream = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    float *dst = buf->datas[0].data;
    if (!dst) { pw_stream_queue_buffer(stream, b); return; }

    /* TODO: Fill dst with decoded PCM from jitter buffer */
    /* For now, fill with silence during integration testing */
    memset(dst, 0, buf->datas[0].maxsize);
    buf->datas[0].chunk->size   = buf->datas[0].maxsize;
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float) * CHANNELS;

    pw_stream_queue_buffer(stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

int pw_sink_run(void) {
    pw_init(NULL, NULL);

    struct pw_main_loop *loop = pw_main_loop_new(NULL);
    struct pw_context   *ctx  = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
    struct pw_core      *core = pw_context_connect(ctx, NULL, 0);

    struct pw_stream *stream = pw_stream_new(core,
        "AetherCodec Hi-Res BT",
        pw_properties_new(
            PW_KEY_NODE_NAME,        "aether_codec_sink",
            PW_KEY_NODE_DESCRIPTION, "AetherCodec Hi-Res Bluetooth",
            PW_KEY_MEDIA_TYPE,       "Audio",
            PW_KEY_MEDIA_CATEGORY,   "Playback",
            PW_KEY_MEDIA_ROLE,       "Music",
            PW_KEY_NODE_LATENCY,     "2048/96000",
            NULL)
    );

    uint8_t buffer[1024];
    struct spa_pod_builder b2 = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b2, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format   = SPA_AUDIO_FORMAT_F32,
            .channels = CHANNELS,
            .rate     = SAMPLE_RATE
        ));

    pw_stream_add_listener(stream, &(struct spa_hook){0}, &stream_events, stream);
    pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                      params, 1);

    printf("[pw_sink] AetherCodec sink registered. Waiting for audio...\n");
    pw_main_loop_run(loop);

    pw_stream_destroy(stream);
    pw_core_disconnect(core);
    pw_context_destroy(ctx);
    pw_main_loop_destroy(loop);
    return 0;
}
```

**Verify:**
```bash
# Run on Laptop B
./aether_receiver &

# Check it appears in PipeWire device list
pw-cli list-objects | grep -A3 "aether"
# or
pactl list sinks | grep AetherCodec
```

---

### Step 4.2 — PipeWire Source Capture on Laptop A

```c
// src/os/pw_source.c
// Captures audio from PipeWire and feeds it to the encoder

static void on_capture(void *userdata) {
    AetherEncoder *enc = userdata;
    struct pw_stream *stream = /* stored somewhere */;
    struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
    if (!b) return;

    float *src = b->buffer->datas[0].data;
    int n_samples = b->buffer->datas[0].chunk->size / (sizeof(float) * CHANNELS);

    /* Convert float to int32_t 24-bit range */
    int32_t pcm[FRAME_SIZE * CHANNELS];
    for (int i = 0; i < n_samples * CHANNELS; i++)
        pcm[i] = (int32_t)(src[i] * (float)(1 << 23));

    /* Encode and send */
    AetherPacket pkt = {0};
    aether_encoder_encode(enc, pcm, n_samples, &pkt);
    /* rfcomm_send_packet(transport, &pkt); */

    pw_stream_queue_buffer(stream, b);
}
```

---

### Step 4.3 — Wire It All Together: aether_sender and aether_receiver

Create `src/daemon/aether_sender.c`:

```c
/* aether_sender -- run on Laptop A
   Usage: aether_sender --target AA:BB:CC:DD:EE:FF --mode nl */

int main(int argc, char *argv[]) {
    /* 1. Parse args */
    /* 2. Connect RFCOMM to Laptop B */
    /* 3. Perform handshake */
    /* 4. Start PipeWire source capture */
    /* 5. Encode loop: capture → encode → send */
    /* 6. Start ABR thread */
}
```

Create `src/daemon/aether_receiver.c`:

```c
/* aether_receiver -- run on Laptop B
   Usage: aether_receiver */

int main(void) {
    /* 1. Register PipeWire sink */
    /* 2. Listen for RFCOMM connection */
    /* 3. Perform handshake */
    /* 4. Start recv thread: receive → jitter buf */
    /* 5. Start decode thread: jitter buf → decode → PipeWire sink */
    /* 6. Start stats thread */
}
```

---

### Step 4.4 — End-to-End System Test

```bash
# Laptop B — start receiver (registers PipeWire sink, listens on RFCOMM)
./aether_receiver

# Laptop A — verify AetherCodec appears as an audio device
pactl list sinks short
# Should show: aether_codec_sink

# Laptop A — play a FLAC file to the AetherCodec sink
pw-play --target aether_codec_sink your_music.flac

# You should hear audio on Laptop B's headphones
```

---

### ✅ Phase 4 Checkpoint

- [x] A visible PipeWire sink is registered — `node.name=aether_codec_sink`,
      `media.class=Audio/Sink`, description "AetherCodec Hi-Res BT", confirmed via
      `pw-play --list-targets`. **It lives on the sender (Laptop A)** — see note below.
- [x] `aether_sender` captures system audio — verified with `--loopback`: 424 frames
      (NL) and 1699 frames (HQ) captured, encoded and decoded from real `pw-play` output
- [x] Lock-free SPSC ring buffer + jitter buffer implemented and unit-tested
      (`test_ring`, `test_jitter`: reorder, duplicates, loss detection)
- [ ] Playing audio on Laptop A produces sound on Laptop B headphones — needs second paired laptop
- [ ] No audible glitches over 60-second sustained playback — needs second paired laptop
- [ ] Mode switching (NL ↔ HQ) without dropout — encoder/decoder reset overlap state on
      switch, but the mid-stream `CTRL_CODEC_CHANGE` signalling path is Phase 5 (ABR)

> **Architecture correction — the sink belongs on the SENDER.** Step 4.1 places the
> virtual sink on Laptop B, but Step 4.4 then runs
> `pw-play --target aether_codec_sink` on Laptop A. Those contradict each other.
> The product goal ("stream from any app on Laptop A, play on Laptop B's
> headphones") only works one way:
>
> - **Laptop A** registers the virtual **sink**; apps play into it → capture → encode → RFCOMM
> - **Laptop B** is a plain **playback** client: RFCOMM → jitter buffer → decode → DAC
>
> **Other Phase 4 corrections:**
>
> - The doc's `pw_sink.c` passes `&(struct spa_hook){0}` to
>   `pw_stream_add_listener` — a compound literal whose lifetime ends at the
>   semicolon, leaving PipeWire with a dangling pointer. Uses a real hook now.
> - It also declares `PW_DIRECTION_OUTPUT` for something called a sink, and never
>   connects the decoder. Rewritten around `pw_thread_loop` + `pw_stream_new_simple`.
> - **Do not set `node.driver=true` on the sink.** A `pw_stream` provides no timing
>   source, so claiming to be a driver can get the node selected to clock the graph,
>   which then stalls (`streaming` state reached but `process()` never fires, and
>   clients hang). Verified experimentally: removing it made capture work immediately.
> - `pw_buffer.requested` only exists from PipeWire 0.3.49; guarded with
>   `PW_CHECK_VERSION` so this builds on 0.3.48.
> - Samples cross the PipeWire boundary as `S32` with an exact `>>8`/`<<8` to the
>   codec's 24-bit range — no float conversion, so NL stays bit-exact.
>
> **New: `--loopback` mode.** `aether_sender --loopback` runs
> capture → encode → decode → playback on a single machine, making the whole Phase 4
> path testable without Laptop B. Add `--no-play` on a machine with no real audio
> output, where our sink would be the default and the playback stream would
> auto-connect back into it (feedback loop).

---

## Phase 5 — Adaptive Bitrate Engine

> **Goal:** Automatically switch codec mode based on Bluetooth signal strength. The codec degrades gracefully when you move the laptops further apart.

---

### Step 5.1 — RSSI Polling

```c
// src/abr/abr_ctrl.c

#include "abr_ctrl.h"
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* Read RSSI for an active ACL connection */
int get_rssi(const char *target_addr, int *rssi_out) {
    int dev_id = hci_get_route(NULL);
    int sock   = hci_open_dev(dev_id);
    if (sock < 0) return -1;

    bdaddr_t bdaddr;
    str2ba(target_addr, &bdaddr);

    struct hci_conn_info_req *cr = calloc(1,
        sizeof(*cr) + sizeof(struct hci_conn_info));
    memcpy(&cr->bdaddr, &bdaddr, sizeof(bdaddr));
    cr->type = ACL_LINK;

    if (ioctl(sock, HCIGETCONNINFO, cr) < 0) {
        free(cr); close(sock); return -1;
    }

    struct hci_request rq = {0};
    struct {
        uint8_t  status;
        uint16_t handle;
        int8_t   rssi;
    } rp = {0};

    uint16_t handle = cr->conn_info->handle;
    rq.ogf    = OGF_STATUS_PARAM;
    rq.ocf    = OCF_READ_RSSI;
    rq.cparam = &handle;
    rq.clen   = sizeof(handle);
    rq.rparam = &rp;
    rq.rlen   = sizeof(rp);

    hci_send_req(sock, &rq, 1000);
    *rssi_out = rp.rssi;

    free(cr);
    close(sock);
    return 0;
}
```

---

### Step 5.2 — ABR State Machine Implementation

```c
// Implement the state machine from the HLD exactly

typedef enum {
    STATE_NL_96K = 0,   // best quality
    STATE_NL_48K = 1,
    STATE_HQ_96K = 2,
    STATE_HQ_48K = 3,   // worst quality (fallback)
} ABRState;

static const char *state_names[] = {
    "NL-96kHz", "NL-48kHz", "HQ-96kHz", "HQ-48kHz"
};

struct ABRCtrl {
    ABRState     current;
    int          good_count;
    int          bad_count;
    uint64_t     last_switch_ms;
    ABRCallback  callback;
    void        *userdata;
    char         target_addr[18];
    pthread_t    thread;
    volatile int running;
};

static ABRState classify_link(int rssi, float loss_pct) {
    if (rssi > -65 && loss_pct < 1.0f)  return STATE_NL_96K;
    if (rssi > -75 && loss_pct < 3.0f)  return STATE_NL_48K;
    if (rssi > -80 && loss_pct < 8.0f)  return STATE_HQ_96K;
    return STATE_HQ_48K;
}

static void *abr_thread(void *arg) {
    ABRCtrl *abr = arg;
    while (abr->running) {
        usleep(500000);  // poll every 500ms

        int rssi = -90;
        get_rssi(abr->target_addr, &rssi);

        /* packet loss is reported by receiver via CTRL_STATS_REPLY */
        float loss_pct = 0.0f;  // TODO: read from stats channel

        ABRState target = classify_link(rssi, loss_pct);

        if (target < abr->current) {         // downgrade
            abr->bad_count++;
            abr->good_count = 0;
            if (abr->bad_count >= 2) {
                printf("[abr] Downgrade: %s → %s (RSSI=%d, loss=%.1f%%)\n",
                       state_names[abr->current], state_names[target], rssi, loss_pct);
                abr->current = target;
                abr->bad_count = 0;
                if (abr->callback) abr->callback(target, 0, abr->userdata);
            }
        } else if (target > abr->current) {  // upgrade
            abr->good_count++;
            abr->bad_count = 0;
            uint64_t now_ms = aether_timestamp_us() / 1000;
            if (abr->good_count >= 3 &&
                now_ms - abr->last_switch_ms >= 3000) {
                printf("[abr] Upgrade: %s → %s (RSSI=%d)\n",
                       state_names[abr->current], state_names[target], rssi);
                abr->current = target;
                abr->good_count = 0;
                abr->last_switch_ms = now_ms;
                if (abr->callback) abr->callback(target, 0, abr->userdata);
            }
        } else {
            abr->good_count = abr->bad_count = 0;
        }
    }
    return NULL;
}
```

---

### Step 5.3 — ABR Integration Test

```bash
# Start streaming in NL-96K mode
# Walk Laptop A progressively further from Laptop B
# Watch console output for automatic mode switches:
# [abr] Downgrade: NL-96kHz → NL-48kHz (RSSI=-72, loss=2.1%)
# [abr] Downgrade: NL-48kHz → HQ-96kHz (RSSI=-78, loss=5.3%)
# Walk back closer — watch upgrades happen with 3s hysteresis
```

---

### ✅ Phase 5 Checkpoint

- [x] ABR downgrades trigger within 1 second — `test_abr`: 2 polls × 500 ms = **1000 ms**
- [x] ABR upgrades wait 3 seconds — `test_abr`: held until `ABR_UPGRADE_HOLD_MS` (3000 ms) elapsed
- [x] Flapping link does not oscillate; stable link fires no spurious switches (`test_abr`)
- [x] Ladder walks NL-96k → NL-48k → HQ-96k → HQ-48k in order (`test_abr`)
- [x] 48 kHz states are functional — 2:1 resampler, `test_resample`: 1 kHz round-trip
      ratio 1.002, 35 kHz rejected at −65.8 dB (no aliasing)
- [x] Live integration verified with `--abr-demo`: 8 transitions over 1782 frames,
      encoder mode/rate followed each switch, zero errors
- [ ] RSSI polling returns correct values — needs an active BT link + `CAP_NET_RAW`
- [ ] Mode switches do not cause audible pops or gaps > 100ms — needs listening test
- [ ] System holds NL-96K at 0–1m range, degrades gracefully at 5m+ — needs second paired laptop

> **Corrections and notes for Phase 5:**
>
> - **The doc's hysteresis comparison is inverted.** With `STATE_NL_96K = 0`
>   (best) … `STATE_HQ_48K = 3` (worst), a degrading link makes `classify_link`
>   return a *larger* value — yet Step 5.2 labels `target > abr->current` as an
>   "upgrade" and `target < current` as a "downgrade". As written the engine
>   raises quality exactly when the link fails. The comparisons are reversed here
>   (`target > current` ⇒ degraded ⇒ downgrade fast).
> - `last_switch_ms` is now updated on **every** commit, not only on upgrades, so
>   the 3 s hold applies after a downgrade too (otherwise a downgrade is
>   immediately followed by an eligible upgrade).
> - **The ladder's bitrates are not monotonic** (1400 → 800 → **1000** → 600 kbps):
>   `NL-48k → HQ-96k` *increases* demand on a worsening link. This is what PRD 4.4
>   specifies (a quality-preference ladder: stay lossless by dropping rate first,
>   then accept lossy at full rate), so it is implemented as written — but it is
>   worth knowing when interpreting a range test.
> - **Sample-rate switching needed a resampler.** Two of the four states are
>   48 kHz, which would otherwise be inert. `src/abr/resample.c` adds a 63-tap
>   linear-phase windowed-sinc 2:1 decimator/interpolator; the sender decimates
>   before encoding and the receiver interpolates after decoding, so the
>   OS-facing format stays 96 kHz throughout. **NL-48K is bit-exact only with
>   respect to the decimated signal** — the 96→48 conversion itself is lossy, so
>   it is a degraded fallback, not a lossless path.
> - **Mode changes need no control packet.** Every `AetherPacket` header already
>   carries `mode` and `sample_rate`, and the decoder flushes its state when they
>   change. That is more robust than the HLD's single `CTRL_CODEC_CHANGE`, which
>   would desynchronise the stream if that one packet were lost.
> - **Not implemented: packet-loss feedback.** `abr_classify` honours the loss
>   thresholds (and `test_abr` covers them), but the receiver's
>   `CTRL_STATS_REPLY` back-channel does not exist, so the live engine drives on
>   RSSI alone.
> - **RSSI failure is not treated as a bad link.** Without `CAP_NET_RAW` (or with
>   no active connection) the daemon warns once and *holds* the current quality;
>   an unreadable RSSI is missing data, not evidence of a weak signal.
> - **New: `--abr-demo`** sweeps a simulated RSSI through the real engine so the
>   ladder can be exercised on a single machine (Step 5.3 otherwise needs two
>   laptops and physical distance).

---

## Phase 6 — End-to-End Demo & Measurement

> **Goal:** Produce a clean, measurable, presentable demo with spectral analysis to compare AetherCodec vs LDAC vs raw.

> **Divergence from this section's original sketch.** The plan as written
> assumed `aether_encode`/`aether_decode` already existed (Step 2.5 used them)
> and that the live `aether_sender` daemon takes `--input FILE`/
> `--record-output FILE` flags. Neither was true: `aether_sender` is a
> PipeWire-driven daemon built around a capture ring buffer (`src/daemon/aether_sender.c`),
> not a file processor, and MANUAL_TESTING.md §4 explicitly flagged the file
> tool as "not built yet". Rather than bolt file-I/O onto the live daemon
> (which would need a second, parallel code path through the ring buffers for
> no real benefit), Phase 6 adds `tools/aether_encode.c` /
> `tools/aether_decode.c` as a **separate, direct file-to-file path**: WAV in,
> `AetherEncoder`/`AetherDecoder` straight through, `.aether` bitstream or WAV
> out. It links only `aether_codec` (no PipeWire, no Bluetooth), so it's the
> fastest way to get an SNR/bitrate number and needs neither a second laptop
> nor a running PipeWire session. The live two-machine path (`aether_sender
> --target` / `aether_receiver`, already built in Phase 4/5) still covers the
> `--verbose` bitrate/loss logging Step 6.3 asked for — see §6.3 below.

---

### Step 6.1 — File-to-file encode/decode + spectral comparison

Three pieces, all under `tools/`:

- **`aether_encode.c`** — `aether_encode input.wav output.aether [--mode nl|hq]`.
  Reads a WAV file (`tools/wav_io.c`: 16/24/32-bit integer PCM, mono/stereo),
  chunks it into `LPC_FRAME_SIZE` (NL) or `MDCT_HOP` (HQ) frames, and feeds
  each through `aether_encoder_encode` exactly like `aether_sender` does. The
  output file is `[magic:u32][total_frames:u32]` followed by packed
  `AetherPacket`s back-to-back — the *same* framing RFCOMM carries, so
  `aether_decode` reads it with the same header-then-payload loop as
  `transport_rfcomm.c`'s `rfcomm_recv_packet`. Prints raw/encoded bytes, ratio,
  and bitrate.
- **`aether_decode.c`** — `aether_decode input.aether output.wav`. Mode,
  sample rate, bit depth and channel count all come back out of the first
  packet's `AetherHeader` — nothing but the original frame count (to trim the
  last frame's zero-padding) needs to travel outside the packet stream.
- **`compare_spectra.py`** — `python3 tools/compare_spectra.py original.wav
  decoded.wav [label] [--shift N] [--no-plot]`. Reports **two** SNRs, not one:
  full-band and audible-band (same 5-tap-MA-×4 low-pass `test_codec_hq.c`
  uses, first null ≈ fs/5). The doc's original sketch computed only a naive
  full-band SNR — per the note in CLAUDE.md, that number is meaningless for HQ
  mode at 96 kHz, where the psychoacoustic model correctly zeroes everything
  above the ATH (~17.6 kHz) and full-band SNR just penalizes it for doing so.
  `--shift 512` compensates for HQ's inherent one-MDCT-hop OLA decode delay
  (see `codec_mdct_dec.c` / `aether_decoder.c`) before comparing — without it
  the two signals are misaligned by 512 samples and the SNR is meaningless
  (verified: an aligned tone came back at **28.4 dB** audible-band, matching
  `test_codec_hq`'s ~27.5 dB reference; unaligned it read −5 dB). NL needs no
  shift — LPC has no cross-frame latency, so decode is sample-for-sample
  aligned with the input. Needs `numpy` (`pip install numpy`); `matplotlib` is
  optional — its absence just skips the spectrogram PNG, not the SNR numbers.

Verified end-to-end during implementation: a 2 s/96 kHz/24-bit/stereo tone
round-tripped through NL came back **byte-for-bit-identical** in the raw PCM
(`sox ... -t raw | cmp` after encode→decode), and separately a mono 16-bit
48 kHz clip of 2578 samples — not a multiple of any frame size — round-tripped
identically too, confirming the last-frame zero-pad/truncate logic.

```bash
cd build
./tools/aether_encode  test_hires.wav  /tmp/nl.aether  --mode nl
./tools/aether_decode  /tmp/nl.aether  /tmp/decoded_nl.wav
python3 ../tools/compare_spectra.py test_hires.wav /tmp/decoded_nl.wav "AetherCodec NL"

./tools/aether_encode  test_hires.wav  /tmp/hq.aether  --mode hq
./tools/aether_decode  /tmp/hq.aether  /tmp/decoded_hq.wav
python3 ../tools/compare_spectra.py test_hires.wav /tmp/decoded_hq.wav "AetherCodec HQ" --shift 512
```

`aether_encode` only reads WAV (no FLAC/MP3 parsing — that's what `sox
input.flac input.wav` is for).

---

### Step 6.2 — Live two-machine demo (needs A + B)

The Phase 4/5 daemons already do everything the original `demo.sh` sketch
wanted, via flags that exist today:

```bash
# 🎧 B — start first
./aether_receiver --verbose

# 🖥️ A — Test 1: Near-Lossless
./aether_sender --target BB:BB:BB:BB:BB:BB --mode nl --verbose
pw-play --target aether_codec_sink test_hires.flac

# 🖥️ A — Test 2: Perceptual HQ (Ctrl+C the sender above first, then)
./aether_sender --target BB:BB:BB:BB:BB:BB --mode hq --verbose
pw-play --target aether_codec_sink test_hires.flac

# 🖥️ A — Test 3: Adaptive bitrate while walking away
./aether_sender --target BB:BB:BB:BB:BB:BB --mode auto --verbose
pw-play --target aether_codec_sink test_hires.flac
```

`aether_sender --verbose` already logs `frames= secs= kbps= mode= rate=`
(§6.3's "sustained bitrate logging" ask) and `aether_receiver --verbose` logs
`recv= played= lost= buffer=…ms underruns=` — both exist today, see
MANUAL_TESTING.md §6.3 and §7.

---

### Step 6.3 — Quality measurement checklist

| # | Measurement | How |
|---|---|---|
| 1 | Bitrate (NL/HQ, both modes) | `aether_encode --mode nl\|hq` prints it directly (single machine); or `aether_sender --verbose` over a live link |
| 2 | SNR full-band / audible-band | `compare_spectra.py` (single machine, §6.1) |
| 3 | Bit-exactness (NL) | `compare_spectra.py` reports `Lossless (>120dB): YES`, or raw-PCM `cmp` as shown in §6.1 |
| 4 | Packet loss / underruns over BT | `aether_receiver --verbose` `lost=`/`underruns=` fields (needs A+B) |
| 5 | Latency | **Needs real hardware** — a click track played on A, captured acoustically or via loopback cable on B, delay measured in samples. Not automatable on one dev machine; no tooling added for this in Phase 6. Record the result manually in the table below once measured. |
| 6 | Frequency response sweep | `sox synth sweep` → `aether_encode`/`decode` → inspect the spectrogram PNG from `compare_spectra.py` |
| 7 | THD+N, MUSHRA listening score, LDAC comparison | **Needs real hardware/listening test** (REW, a second BT device that actually speaks A2DP/LDAC, human listeners) — out of scope for this codebase's tooling; record manually |

---

### Step 6.4 — Comparison Table (Fill In After Measurement)

| Metric | AetherCodec NL | AetherCodec HQ | LDAC 990k | Target |
|---|---|---|---|---|
| Effective bitrate | ___ kbps | ___ kbps | 990 kbps | >900 kbps |
| Lossless | YES / NO | NO | NO | NL: YES |
| SNR (audible-band) | ___ dB | ___ dB | ~85 dB | NL: >120 dB |
| Latency | ___ ms | ___ ms | ~200 ms | <80 ms |
| Freq response | ±___ dB | ±___ dB | ±0.5 dB | ±0.2 dB |
| Packet loss at 2m | ___% | ___% | N/A | <0.5% |

---

### ✅ Phase 6 Final Checkpoint

- [x] `aether_encode`/`aether_decode` built and verified: NL round-trip is
      bit-for-bit identical raw PCM (24-bit stereo tone, and a mono 16-bit
      odd-length clip); HQ round-trip (with `--shift 512`) measured
      **28.4 dB** audible-band SNR on a tonal signal, matching
      `test_codec_hq`'s ~27.5 dB reference
- [x] `compare_spectra.py` built: reports full-band + audible-band SNR and
      (with `matplotlib`) a 3-panel spectrogram PNG; verified against both NL
      and HQ output on this machine
- [ ] Full 60-second music playback with zero dropouts in good BT conditions — needs second laptop
- [ ] NL mode SNR > 120 dB on real music (verified on a synthetic tone above; re-verify on a real track once available)
- [ ] HQ mode sounds transparent on headphones (MUSHRA score target: >85) — needs a listening test, pending
- [ ] ABR demo works: mode changes audibly gracefully during range test — needs second laptop (single-machine `--abr-demo` already verified in Phase 5)
- [ ] Latency measured and recorded: ___ ms — needs real hardware, pending
- [x] Bitrate logged and confirmed within target range — see `aether_encode` output above and Phase 3's `test_codec_hq` numbers (900–1,100 kbps HQ broadband target)

---

## Quick Reference: Build & Run

```bash
# Build everything
cd ~/aethercodec && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DAETHER_ENABLE_PIPEWIRE=ON -DAETHER_ENABLE_ALSA=OFF
make -j$(nproc)

# Run tests
ctest --output-on-failure

# Start receiver (Laptop B)
./src/aether_receiver --verbose

# Start sender (Laptop A — replace with Laptop B's BT address)
./src/aether_sender --target AA:BB:CC:DD:EE:FF --mode nl --verbose

# Single-machine file-based quality check (no second laptop needed)
./tools/aether_encode  input.wav /tmp/out.aether --mode nl
./tools/aether_decode  /tmp/out.aether /tmp/decoded.wav
python3 ../tools/compare_spectra.py input.wav /tmp/decoded.wav "AetherCodec NL"
```

> There is no `aether_status` tool — link/quality status is what
> `aether_sender --verbose` / `aether_receiver --verbose` print
> (`[stats] frames= kbps= mode= rate=` / `recv= played= lost= underruns=`).

---

## Common Issues & Fixes

| Problem | Likely Cause | Fix |
|---|---|---|
| `connect: Connection refused` | Laptop B RFCOMM not listening | Start `aether_receiver` first |
| `bind: Address already in use` | RFCOMM channel occupied | Kill old process: `pkill aether_receiver` |
| Throughput < 500 kbps | BT adapter is BT 2.0 (no EDR) | Lower bitrate target; use HQ mode only |
| Audio choppy / clicking | Jitter buffer too small | Increase `target_ms` in `jitter_buf_create()` |
| LPC test mismatches | Integer overflow in autocorr | Use `int64_t` accumulator for autocorrelation |
| MDCT inverse not matching | Missing overlap-add | Ensure 50% overlap-add in iMDCT |
| PipeWire sink not visible | PipeWire service not running | `systemctl --user start pipewire pipewire-pulse` |
| RSSI always -90 | HCI socket permission denied | `sudo setcap cap_net_raw+ep ./aether_sender` |

---

*AetherCodec Implementation Plan v1.0 — Open Source — Built in C*  
*Update this document as you complete each step. Check off each box as you go.*
