/* Full-frame integration test: PCM -> aether_encoder -> AetherPacket ->
   aether_decoder -> PCM, verifying bit-perfect stereo reconstruction and
   packet CRC integrity. */
#include "aether_encoder.h"
#include "aether_decoder.h"
#include "aether_packet.h"
#include "codec_lpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define FRAME 2048

int main(void) {
    srand(7);

    /* Build an interleaved stereo frame: L = tone, R = tone + noise. */
    int32_t pcm_in[FRAME * 2];
    for (int i = 0; i < FRAME; i++) {
        int32_t l = (int32_t)(6000000.0 * sin(2.0 * 3.14159 * 220.0 * i / 96000.0));
        int32_t nz = (rand() % 4096) - 2048;
        int32_t rr = (int32_t)(4000000.0 * sin(2.0 * 3.14159 * 660.0 * i / 96000.0)) + nz;
        pcm_in[i * 2]     = l;
        pcm_in[i * 2 + 1] = rr;
    }

    AetherEncoder *enc = aether_encoder_create(AETHER_MODE_NL, 96000, 24, 2);
    AetherDecoder *dec = aether_decoder_create();
    assert(enc && dec);

    AetherPacket pkt;
    int r = aether_encoder_encode(enc, pcm_in, FRAME, &pkt);
    assert(r == 0);
    assert(pkt.hdr.mode == AETHER_MODE_NL);
    assert(pkt.hdr.channels == 2);

    /* Serialize + validate CRC like the transport would. */
    static uint8_t wire[AETHER_HEADER_SIZE + AETHER_MAX_PAYLOAD + 4];
    int wlen = aether_packet_pack(&pkt, wire, sizeof(wire));
    assert(wlen > 0);
    AetherPacket rx;
    assert(aether_packet_unpack(wire, wlen, &rx) == 0);

    int32_t pcm_out[FRAME * 2];
    int got = aether_decoder_decode(dec, &rx, pcm_out, FRAME * 2);
    assert(got == FRAME * 2);

    int mismatches = 0;
    for (int i = 0; i < FRAME * 2; i++)
        if (pcm_in[i] != pcm_out[i]) {
            if (mismatches < 10)
                fprintf(stderr, "  MISMATCH at %d: in=%d out=%d\n",
                        i, pcm_in[i], pcm_out[i]);
            mismatches++;
        }
    assert(mismatches == 0);

    int raw = FRAME * 2 * 3;  // 24-bit stereo
    printf("\xE2\x9C\x93 NL codec integration: LOSSLESS stereo round-trip (0 mismatches)\n");
    printf("  Frame raw: %d bytes | packet payload: %u bytes | ratio: %.2fx\n",
           raw, pkt.hdr.payload_size, (double)raw / pkt.hdr.payload_size);

    aether_encoder_destroy(enc);
    aether_decoder_destroy(dec);
    return 0;
}
