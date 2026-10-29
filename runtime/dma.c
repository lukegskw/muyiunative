// The DMA controller (psx-spx, DMA Channels): channels 0..6 (MDEC in, MDEC out, GPU, CD-ROM, SPU, PIO, OTC) move words between main RAM and the devices; DPCR enables
// them, DICR collects their completions into IRQ 3. Sync mode 0 moves a block at once, mode 1 a number of blocks, mode 2 (GPU only) follows a linked list of packets.
// A transfer runs whole when started, and the CPU, which waits while the DMA owns the bus, is charged a cycle per word. MDEC and PIO transfers come in phase 2
#include "ps1.h"
#include <string.h>

void dma_reset(ps1 *m) { memset(m->dma, 0, sizeof m->dma); m->dpcr = 0x07654321; m->dicr = 0; }

static uint32_t *ram32(ps1 *m, uint32_t a) { return (uint32_t *)(m->ram + (a & 0x1ffffc)); }

static void dicr_update(ps1 *m) {   // bit 31 is the master flag; IRQ 3 is raised when it rises
    uint32_t d = m->dicr, was = d >> 31;
    int on = d & 0x8000 || (d & 0x800000 && d >> 16 & d >> 24 & 0x7f);
    m->dicr = (d & 0x7fffffff) | (uint32_t)on << 31;
    if (on && !was) irq_raise(m, IRQ_DMA);
}

static void to_device(ps1 *m, int ch, uint32_t w) {
    if (ch == 2) gpu_gp0(m, w);
    else if (ch == 4) spu_dma_write(m, w);
    else RED_ONCE(m, RED_DMA, "dma: channel %d is not modelled yet (phase 2)", ch);
}

static uint32_t from_device(ps1 *m, int ch) {
    if (ch == 2) return gpu_read(m);
    if (ch == 3) return cd_dma_read(m);
    if (ch == 4) return spu_dma_read(m);
    RED_ONCE(m, RED_DMA, "dma: channel %d is not modelled yet (phase 2)", ch);
    return 0;
}

static void run(ps1 *m, int ch) {
    ps1_dma *d = &m->dma[ch];
    uint32_t mode = d->chcr >> 9 & 3, addr = d->madr & 0x1ffffc, step = d->chcr & 2 ? (uint32_t)-4 : 4, words = 0;
    if (ch == 6) {   // an empty ordering table: each entry points to the one before it, the first holds the end code (psx-spx, GPU Depth Ordering)
        uint32_t n = d->bcr & 0xffff ? d->bcr & 0xffff : 0x10000;
        for (uint32_t i = 0; i < n; i++, addr -= 4) *ram32(m, addr) = i == n - 1 ? 0xffffff : (addr - 4) & 0x1fffff;
        words = n;
    } else if (mode == 2) {
        for (uint32_t guard = 0; ch == 2; guard++) {   // packets of a header (word count, next address) and GP0 words
            uint32_t h = *ram32(m, addr), n = h >> 24;
            for (uint32_t i = 1; i <= n; i++) gpu_gp0(m, *ram32(m, addr + 4 * i));
            words += n + 1;
            if (h & 0x800000) break;   // the end code (00FFFFFFh in practice)
            if (guard == 0x100000) { RED("dma: the GPU list at %06x does not end; stopped", d->madr); break; }
            addr = h & 0x1ffffc;
        }
        d->madr = 0xffffff;
    } else {
        uint32_t n = mode == 0 ? (d->bcr & 0xffff ? d->bcr & 0xffff : 0x10000) : (d->bcr & 0xffff) * (d->bcr >> 16);
        for (uint32_t i = 0; i < n; i++, addr += step) { if (d->chcr & 1) to_device(m, ch, *ram32(m, addr)); else *ram32(m, addr) = from_device(m, ch); }
        words = n;
        if (mode == 1) { d->madr = addr & 0xffffff; d->bcr &= 0xffff; }
    }
    d->chcr &= ~0x11000000u;
    m->cycles += words;
    if (m->dicr & 1u << (16 + ch)) m->dicr |= 1u << (24 + ch);
    dicr_update(m);
}

static void maybe_run(ps1 *m, int ch) {   // started by CHCR bit 24, and in sync mode 0 also by the trigger bit 28, on a channel DPCR enables
    uint32_t c = m->dma[ch].chcr;
    if (c & 0x01000000 && (c >> 9 & 3 || c & 0x10000000) && m->dpcr >> (4 * ch + 3) & 1) run(m, ch);
}

uint32_t dma_read(ps1 *m, uint32_t off) {
    if (off == 0x70) return m->dpcr;
    if (off == 0x74) return m->dicr;
    int ch = off >> 4;
    if (ch > 6) return 0;
    switch (off & 0xf) { case 0: return m->dma[ch].madr; case 4: return m->dma[ch].bcr; case 8: return m->dma[ch].chcr; }
    return 0;
}

void dma_write(ps1 *m, uint32_t off, uint32_t v) {
    if (off == 0x70) { m->dpcr = v; for (int ch = 0; ch < 7; ch++) maybe_run(m, ch); return; }
    if (off == 0x74) { m->dicr = (m->dicr & 0x7f000000 & ~v) | (v & 0x00ff803f); dicr_update(m); return; }   // a 1 written to a flag acknowledges it
    int ch = off >> 4;
    if (ch > 6) return;
    ps1_dma *d = &m->dma[ch];
    switch (off & 0xf) {
    case 0: d->madr = v & 0xffffff; break;
    case 4: d->bcr = v; break;
    case 8: d->chcr = ch == 6 ? (v & 0x51000000) | 2 : v & 0x71770703; maybe_run(m, ch); break;   // OTC always runs backwards, to RAM
    }
}
