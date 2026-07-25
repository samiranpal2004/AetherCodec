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

#endif /* CODEC_LPC_H */
