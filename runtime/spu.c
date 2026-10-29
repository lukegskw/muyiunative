// The SPU's registers and sound RAM (psx-spx, Sound Processing Unit): no sound is made yet (phase 2). The registers keep what is written; SPUSTAT mirrors SPUCNT's mode
// bits at once and shows no transfer in progress, as transfers complete as they are written. Sound RAM is written through the data port (1F801DA8h) and DMA 4, from the
// transfer address (1F801DA6h, in 8-byte units)
#include "ps1.h"
#include <string.h>

#define S (&m->spu)
enum { R_KON = 0x188, R_ADDR = 0x1a6, R_DATA = 0x1a8, R_CNT = 0x1aa, R_STAT = 0x1ae };

void spu_reset(ps1 *m) { memset(S->reg, 0, sizeof S->reg); S->addr = 0; }

static void put(ps1 *m, uint16_t v) { memcpy(&S->ram[S->addr], &v, 2); S->addr = (S->addr + 2) & (SPU_RAM_SIZE - 1); }
static uint16_t get(ps1 *m) { uint16_t v; memcpy(&v, &S->ram[S->addr], 2); S->addr = (S->addr + 2) & (SPU_RAM_SIZE - 1); return v; }

uint16_t spu_read(ps1 *m, uint32_t off) {
    off &= 0x3fe;
    if (off == R_STAT) return S->reg[R_CNT / 2] & 0x3f;
    if (off < 0x180 && (off & 0xf) == 0xc) return 0;   // a voice's current envelope volume: silent
    return S->reg[off / 2];
}

void spu_write(ps1 *m, uint32_t off, uint16_t v) {
    off &= 0x3fe;
    S->reg[off / 2] = v;
    if (off == R_ADDR) S->addr = (uint32_t)v * 8 & (SPU_RAM_SIZE - 1);
    else if (off == R_DATA) put(m, v);
    else if ((off == R_KON || off == R_KON + 2) && v) RED_ONCE(m, RED_SPU_VOICE, "spu: voices are keyed on, but no sound is made yet (phase 2)");
}

void spu_dma_write(ps1 *m, uint32_t w) { put(m, (uint16_t)w); put(m, (uint16_t)(w >> 16)); }
uint32_t spu_dma_read(ps1 *m) { uint32_t lo = get(m); return lo | (uint32_t)get(m) << 16; }
