// Streaming video decoder (plan.md, phase 2), from psx-spx, Macroblock Decoder: MDEC I/O Ports, Commands, Decompression, and Data Format. Synthetic t_mdec sends split
// RLE packets, custom scale/quant tables, and multiple blocks while the output is blocked: input must stop at its 32-word capacity instead of consuming an entire image.
// The low-level two-pass IDCT honors the uploaded scale table. Its published rounding is explicitly approximate, as are the published YUV coefficients; exact rounding
// and decode latency remain open and are reported once. One 8x8 output block at a time is packed into a bounded queue; DMA1's 16x16 rearrangement uses per-word metadata
// https://psx-spx.consoledev.net/macroblockdecodermdec/#mdec-decompression
#include "ps1.h"
#include "mdec.h"
#include <string.h>

#define M (&m->mdec)
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int signed10(uint16_t v) { int n = v & 1023; return n & 512 ? n - 1024 : n; }
static int signed9(int n) { n &= 511; return n & 256 ? n - 512 : n; }
static int mono(const ps1_mdec *d) { return d->depth < 2; }
static int block(const ps1_mdec *d) { return mono(d) ? 4 : (d->stage + 4) % 6; }
static int words(const ps1_mdec *d) { return d->depth == 0 ? 8 : d->depth == 1 ? 16 : d->depth == 2 ? 48 : 32; }

// Decompression, zigzag[] gives natural position -> scan index. Its inverse is used only when q_scale is nonzero; q_scale=0 keeps coefficient order linear
static int zigzag(int scan) {
    static const uint8_t order[64] = {0,1,5,6,14,15,27,28,2,4,7,13,16,26,29,42,3,8,12,17,25,30,41,43,9,11,18,24,31,40,44,53,10,19,23,32,39,45,52,54,20,22,33,38,46,51,55,60,21,34,37,47,50,56,59,61,35,36,48,49,57,58,62,63};
    for (int p = 0; p < 64; p++) if (order[p] == scan) return p;
    return 0;
}
static void idct(ps1 *m) {
    int16_t tmp[64]; int16_t *src = M->coeff, *dst = tmp;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (int z = 0; z < 8; z++) sum += (int64_t)src[y + z * 8] * (M->scale[x + z * 8] >> 3);
            dst[x + y * 8] = (int16_t)((sum + 0xfff) >> 13);
        }
        int16_t *swap = src; src = dst; dst = swap;
    }
}
static void emit(ps1 *m) {
    uint8_t bytes[192] = {0}; int n = 0, output_block = block(M);
    for (int p = 0; p < 64; p++) {
        if (mono(M)) {
            uint8_t y = (uint8_t)clamp(signed9(M->coeff[p]), -128, 127); if (!M->sign) y ^= 0x80;
            if (!M->depth) bytes[p / 2] |= (uint8_t)((y >> 4) << ((p & 1) * 4)); else bytes[p] = y;
        } else {
            int bx = output_block & 1, by = output_block >> 1, x = p & 7, y = p >> 3;
            int uv = (bx * 8 + x) / 2 + (by * 8 + y) / 2 * 8, cr = M->cr[uv], cb = M->cb[uv], yy = M->coeff[p];
            int v[3] = {yy + ((1436 * cr) >> 10), yy + ((-352 * cb - 731 * cr) >> 10), yy + ((1815 * cb) >> 10)};
            uint8_t rgb[3]; for (int c = 0; c < 3; c++) { rgb[c] = (uint8_t)clamp(v[c], -128, 127); if (!M->sign) rgb[c] ^= 0x80; }
            if (M->depth == 2) { bytes[p * 3] = rgb[0]; bytes[p * 3 + 1] = rgb[1]; bytes[p * 3 + 2] = rgb[2]; }
            else { uint16_t color = (uint16_t)(rgb[0] >> 3 | (rgb[1] >> 3) << 5 | (rgb[2] >> 3) << 10 | M->stp << 15); bytes[p * 2] = (uint8_t)color; bytes[p * 2 + 1] = (uint8_t)(color >> 8); }
        }
    }
    n = words(M);
    for (int i = 0; i < n; i++) {
        unsigned tail = (M->out_head + M->out_n) & 63;
        M->output[tail] = (uint32_t)bytes[i * 4] | (uint32_t)bytes[i * 4 + 1] << 8 | (uint32_t)bytes[i * 4 + 2] << 16 | (uint32_t)bytes[i * 4 + 3] << 24;
        M->out_block[tail] = (uint8_t)output_block; M->out_pos[tail] = (uint8_t)i; M->out_depth[tail] = (uint8_t)M->depth; M->out_n++;
    }
}
static void finish_block(ps1 *m) {
    idct(m);
    if (!mono(M) && M->stage < 2) memcpy(M->stage ? M->cb : M->cr, M->coeff, sizeof M->coeff);
    else emit(m);
    M->active = 0; if (!mono(M)) M->stage = (M->stage + 1) % 6;
}
static void pump(ps1 *m) {
    while (M->busy && M->in_n) {
        uint16_t h = M->input[M->in_head];
        if (M->cmd == 1 && M->active && M->k + (h >> 10) + 1 > 63 && (mono(M) || M->stage >= 2) && 64 - M->out_n < words(M)) break;
        M->in_head = (M->in_head + 1) & 63; M->in_n--;
        if (M->cmd == 2) {
            int p = M->table_pos++; M->quant[p / 32][p % 32 * 2] = (uint8_t)h; M->quant[p / 32][p % 32 * 2 + 1] = (uint8_t)(h >> 8);
        } else if (M->cmd == 3) M->scale[M->table_pos++] = (int16_t)h;
        else if (!M->active) {
            if (h == 0xfe00) continue;
            M->active = 1; M->k = 0; M->q = h >> 10; memset(M->coeff, 0, sizeof M->coeff);
            int q = mono(M) || M->stage >= 2 ? 0 : 1;
            M->coeff[0] = (int16_t)clamp(signed10(h) * (M->q ? M->quant[q][0] : 2), -1024, 1023);
        } else {
            M->k += (h >> 10) + 1;
            if (M->k > 63) finish_block(m);
            else {
                int q = mono(M) || M->stage >= 2 ? 0 : 1;
                int value = M->q ? (signed10(h) * M->quant[q][M->k] * M->q + 4) >> 3 : signed10(h) * 2;
                M->coeff[M->q ? zigzag(M->k) : M->k] = (int16_t)clamp(value, -1024, 1023);
            }
        }
    }
    if (M->busy && !M->remaining && !M->in_n) M->busy = 0;
}
void mdec_reset(ps1 *m) { memset(M, 0, sizeof *M); }
int mdec_dma_ready(ps1 *m, int channel) {
    if (!channel) return !!(M->control & 0x40000000) && M->in_n <= 62 && (!M->busy || M->remaining);
    // I/O Ports documents DREQ1 withdrawing part-way through an output block, without an exact threshold. Available-word gating leaves those FIFO pulse timings open
    return channel == 1 && !!(M->control & 0x20000000) && M->out_n;
}
int32_t mdec_dma_offset(ps1 *m) {
    if (!M->out_n) return 0;
    int i = M->out_head, depth = M->out_depth[i]; if (depth < 2) return 0;
    int b = M->out_block[i], pos = M->out_pos[i] * 4, row = depth == 2 ? 24 : 16;
    int placed = ((b >> 1) * 8 + pos / row) * row * 2 + (b & 1) * row + pos % row;
    return placed - (b * row * 8 + pos);
}
uint32_t mdec_dma_read(ps1 *m) {
    if (!M->out_n) return 0;   // MDEC I/O Ports: an empty data register returns unspecified garbage
    uint32_t v = M->output[M->out_head]; M->out_head = (M->out_head + 1) & 63; M->out_n--; pump(m); return v;
}
uint32_t mdec_read(ps1 *m, uint32_t off) {
    if (!off) return mdec_dma_read(m);
    uint32_t status = (M->out_n ? 0 : 1u << 31) | (M->in_n >= 63 || (M->busy && !M->remaining) ? 1u << 30 : 0) | (uint32_t)M->busy << 29;
    status |= (uint32_t)mdec_dma_ready(m, 0) << 28 | (uint32_t)mdec_dma_ready(m, 1) << 27;
    return status | ((M->command >> 2) & 0x07800000) | (uint32_t)(M->out_n ? M->out_block[M->out_head] : block(M)) << 16 | M->count;
}
void mdec_write(ps1 *m, uint32_t off, uint32_t v) {
    if (off) { if (v & 0x80000000) mdec_reset(m); M->control = v & 0x60000000; return; }
    if (!M->busy) {
        M->command = v; M->cmd = (int)(v >> 29); M->depth = (v >> 27) & 3; M->sign = (v >> 26) & 1; M->stp = (v >> 25) & 1;
        M->table_pos = M->stage = M->active = 0; M->in_head = M->in_n = 0;
        M->remaining = M->cmd == 1 ? v & 0xffff : M->cmd == 2 ? (v & 1 ? 32 : 16) : M->cmd == 3 ? 32 : 0;
        M->count = (uint16_t)(M->cmd >= 1 && M->cmd <= 3 ? M->remaining - 1 : v);
        M->busy = M->remaining != 0;
        if (M->cmd == 1) RED_ONCE(m, RED_MDEC, "mdec: exact IDCT/YUV rounding and FIFO/decode timing are not verified by the public specification");
    } else if (M->remaining && M->in_n <= 62) {
        unsigned tail = (M->in_head + M->in_n) & 63; M->input[tail] = (uint16_t)v; M->input[(tail + 1) & 63] = (uint16_t)(v >> 16); M->in_n += 2;
        M->remaining--; M->count = (uint16_t)(M->remaining - 1); pump(m);
    } else RED_ONCE(m, RED_MDEC, "mdec: input written without FIFO capacity or remaining parameters");
}
