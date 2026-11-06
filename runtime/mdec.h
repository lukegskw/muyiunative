// Streaming macroblock decoder (plan.md, phase 2); psx-spx, Macroblock Decoder: two FIFOs, RLE, quantization, IDCT, and pixel output
#pragma once
#include <stdint.h>
typedef struct ps1_mdec {
    uint32_t command, control, remaining; uint16_t count;
    uint16_t input[64]; uint8_t in_head, in_n;
    uint32_t output[64]; uint8_t out_block[64], out_pos[64], out_depth[64], out_head, out_n;
    uint8_t quant[2][64]; int16_t scale[64], coeff[64], cr[64], cb[64];
    int cmd, busy, depth, sign, stp, stage, active, k, q, table_pos;
    uint64_t until, next, ready[64];   // decoder pipeline, next availability/completion event, and output-word availability times
} ps1_mdec;
struct ps1;
void mdec_reset(struct ps1 *m);   // abort decoding and clear FIFOs/status; retain the uploaded quantization and scale tables
void mdec_event(struct ps1 *m);   // processing made output available or finished the last block; re-evaluate DMA requests
uint32_t mdec_read(struct ps1 *m, uint32_t off);
void mdec_write(struct ps1 *m, uint32_t off, uint32_t value);
int mdec_dma_ready(struct ps1 *m, int channel);
uint32_t mdec_dma_read(struct ps1 *m);
int32_t mdec_dma_offset(struct ps1 *m);   // byte delta added to sequential DMA1 address BEFORE reading the current word
