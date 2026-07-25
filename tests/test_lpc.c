#include "codec_lpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

/* Generous Rice buffer: worst realistic case (~k + few bits/sample) stays well
   under this. rice_encode also fails safe (returns -1) rather than overrunning. */
#define RICE_BUF_BYTES (LPC_FRAME_SIZE * 8)

static int roundtrip_lossless(const int32_t *original, const char *label) {
    LPCFrameHeader hdr = {0};
    int32_t residuals[LPC_FRAME_SIZE] = {0};
    int r = lpc_encode_frame(original, LPC_FRAME_SIZE, &hdr, residuals);
    assert(r == 0);

    int k = rice_select_param(residuals + hdr.order, LPC_FRAME_SIZE - hdr.order);
    uint8_t bitstream[RICE_BUF_BYTES];
    int bytes = rice_encode(residuals + hdr.order, LPC_FRAME_SIZE - hdr.order,
                            k, bitstream, sizeof(bitstream));
    assert(bytes > 0);

    int32_t dec_res[LPC_FRAME_SIZE] = {0};
    r = rice_decode(bitstream, bytes, k, LPC_FRAME_SIZE - hdr.order,
                    dec_res + hdr.order);
    assert(r == 0);

    int32_t decoded[LPC_FRAME_SIZE] = {0};
    r = lpc_decode_frame(&hdr, dec_res, LPC_FRAME_SIZE, decoded);
    assert(r == 0);

    int mismatches = 0;
    for (int i = 0; i < LPC_FRAME_SIZE; i++) {
        if (original[i] != decoded[i]) {
            if (mismatches < 10)
                fprintf(stderr, "  MISMATCH at i=%d: orig=%d decoded=%d\n",
                        i, original[i], decoded[i]);
            mismatches++;
        }
    }
    assert(mismatches == 0);

    int raw_bytes = LPC_FRAME_SIZE * 3;  // 24-bit = 3 bytes/sample
    printf("\xE2\x9C\x93 LPC round-trip (%s): LOSSLESS (0 mismatches), order=%d k=%d\n",
           label, hdr.order, k);
    printf("  Raw: %d bytes | Compressed: %d bytes | Ratio: %.2fx\n",
           raw_bytes, bytes, (double)raw_bytes / bytes);
    return 0;
}

int main(void) {
    srand(42);

    /* Test 1: Synthetic sine wave (LPC's best case) */
    int32_t sine[LPC_FRAME_SIZE];
    for (int i = 0; i < LPC_FRAME_SIZE; i++)
        sine[i] = (int32_t)(8000000.0 * sin(2.0 * 3.14159 * 440.0 * i / 96000.0));
    roundtrip_lossless(sine, "sine");

    /* Test 2: Random noise (LPC's worst case) */
    int32_t noise[LPC_FRAME_SIZE];
    for (int i = 0; i < LPC_FRAME_SIZE; i++)
        noise[i] = (rand() % (1 << 23)) - (1 << 22);
    roundtrip_lossless(noise, "noise");

    /* Test 3: Silence (R[0] == 0 path) */
    int32_t silence[LPC_FRAME_SIZE];
    memset(silence, 0, sizeof(silence));
    roundtrip_lossless(silence, "silence");

    printf("\nAll LPC round-trip tests passed — bit-perfect reconstruction.\n");
    return 0;
}
