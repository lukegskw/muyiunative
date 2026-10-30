// The DMA controller (psx-spx, DMA Channels): channels 0..6 (MDEC in, MDEC out, GPU, CD-ROM, SPU, PIO, OTC) move words between main RAM and the devices; DPCR enables
// them, DICR collects their completions into IRQ 3. Sync mode 0 moves a block at once, mode 1 a number of blocks, mode 2 (GPU only) follows a linked list of packets.
// GPU/CD/SPU/OTC run whole when started; MDEC's two channels alternate as their FIFO requests allow. The CPU is charged a cycle per word (device waitstates remain open).
// PIO has nothing connected. Public reference: https://psx-spx.consoledev.net/dmachannels/ and Macroblock Decoder, DMA
#include "ps1.h"
#include <string.h>

void dma_reset(ps1 *m) { memset(m->dma, 0, sizeof m->dma); m->dpcr = 0x07654321; m->dicr = 0; }

static uint32_t *ram32(ps1 *m, uint32_t a) { return (uint32_t *)(m->ram + (a & 0x1ffffc)); }

static void dicr_update(ps1 *m) {   // bit 31 is the master flag; IRQ 3 is raised when it rises
    uint32_t d = m->dicr, was = d >> 31;
    int on = d & 0x8000 || (d & 0x800000 && d & 0x7f000000);
    m->dicr = (d & 0x7fffffff) | (uint32_t)on << 31;
    if (on && !was) irq_raise(m, IRQ_DMA);
}

static void complete(ps1 *m, int ch) {
    m->dma[ch].active = 0; m->dma[ch].chcr &= ~0x11000000u;
    // t_dma: a completion with master disabled wrongly latched a flag; disabling a channel mask also wrongly hid an existing flag. psx-spx, DICR: both enables gate
    // generation; once latched, flags contribute to the master output independently of the current channel mask, until acknowledged
    if ((m->dicr & 0x800000) && (m->dicr & 1u << (16 + ch))) m->dicr |= 1u << (24 + ch);
    dicr_update(m);
}

// A whole DMA0 transfer can fill the decoder before DMA1 is started. t_mdec streams more than its FIFOs can hold: busy must remain set at backpressure, and starting
// DMA1 must let both channels finish. DMA Channels, Slice mode and MDEC DMA: requests gate each slice; output block metadata controls the 8x8-to-16x16 RAM layout
void dma_resume(ps1 *m) {
    for (;;) {
        int chosen = -1;
        for (int ch = 0; ch < 2; ch++) {
            ps1_dma *d = &m->dma[ch];
            if (!d->active || !(m->dpcr >> (4 * ch + 3) & 1) || !mdec_dma_ready(m, ch)) continue;
            if (chosen < 0 || (m->dpcr >> (4 * ch) & 7) <= (m->dpcr >> (4 * chosen) & 7)) chosen = ch;
        }
        if (chosen < 0) return;
        ps1_dma *d = &m->dma[chosen];
        if (!chosen) mdec_write(m, 0, *ram32(m, d->addr));
        else { int32_t offset = mdec_dma_offset(m); *ram32(m, d->addr + (uint32_t)offset) = mdec_dma_read(m); }
        d->addr = (d->addr + (d->chcr & 2 ? (uint32_t)-4 : 4)) & 0xffffff;
        d->left--; d->block_left--; m->cycles++;
        if (!d->block_left) {
            if (d->chcr >> 9 & 3) { d->blocks--; d->bcr = (d->bcr & 0xffff) | (d->blocks & 0xffff) << 16; d->madr = d->addr; }
            d->block_left = d->block_size;
        }
        if (!d->left) complete(m, chosen);
    }
}

static void mdec_begin(ps1 *m, int ch) {
    ps1_dma *d = &m->dma[ch]; uint32_t mode = d->chcr >> 9 & 3;
    if (mode > 1 || (d->chcr & 1) != (uint32_t)!ch) { RED_ONCE(m, RED_DMA, "dma: unsupported MDEC channel direction or sync mode"); return; }
    if (!d->active) {
        d->block_size = d->bcr & 0xffff ? d->bcr & 0xffff : 0x10000;
        d->blocks = mode ? (d->bcr >> 16 ? d->bcr >> 16 : 0x10000) : 1;
        d->left = (uint64_t)d->block_size * d->blocks; d->block_left = d->block_size;
        d->addr = d->madr & 0xfffffc; d->active = 1; d->chcr &= ~0x10000000u;
    }
    dma_resume(m);
}

static void to_device(ps1 *m, int ch, uint32_t w) {
    if (ch == 2) gpu_gp0(m, w);
    else if (ch == 4) spu_dma_write(m, w);
    else RED_ONCE(m, RED_DMA, "dma: channel %d towards its device: nothing takes the words (PIO has nothing connected, the CD-ROM only sends)", ch);
}

static uint32_t from_device(ps1 *m, int ch) {
    if (ch == 2) return gpu_read(m);
    if (ch == 3) return cd_dma_read(m);
    if (ch == 4) return spu_dma_read(m);
    RED_ONCE(m, RED_DMA, "dma: channel %d from its device: nothing sends, RAM gets 0 (PIO has nothing connected)", ch);
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
            // The hardware log of ps1-tests dma/chain-looping: a list that comes back on itself never completes, nor raises its IRQ
            if (guard == 0x100000) { RED("dma: the GPU list at %06x does not end: the channel stays busy", d->madr); return; }
            addr = h & 0x1ffffc;
        }
        d->madr = 0xffffff;
    } else {
        uint32_t n = mode == 0 ? (d->bcr & 0xffff ? d->bcr & 0xffff : 0x10000) : (d->bcr & 0xffff) * (d->bcr >> 16);
        for (uint32_t i = 0; i < n; i++, addr += step) { if (d->chcr & 1) to_device(m, ch, *ram32(m, addr)); else *ram32(m, addr) = from_device(m, ch); }
        words = n;
        if (mode == 1) { d->madr = addr & 0xffffff; d->bcr &= 0xffff; }
    }
    m->cycles += words;
    complete(m, ch);
}

static void maybe_run(ps1 *m, int ch) {   // started by CHCR bit 24, and in sync mode 0 also by the trigger bit 28, on a channel DPCR enables
    uint32_t c = m->dma[ch].chcr;
    if (c & 0x01000000 && (c >> 9 & 3 || c & 0x10000000 || m->dma[ch].active) && m->dpcr >> (4 * ch + 3) & 1) {
        if (ch < 2) mdec_begin(m, ch); else run(m, ch);
    }
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
    if (off == 0x74) {   // t_dma: keep the old master output until recomputed, so writes that leave it high cannot retrigger IRQ3; a 1 acknowledges each flag
        m->dicr = (m->dicr & 0x80000000) | (m->dicr & 0x7f000000 & ~v) | (v & 0x00ff803f); dicr_update(m); return;
    }
    int ch = off >> 4;
    if (ch > 6) return;
    ps1_dma *d = &m->dma[ch];
    switch (off & 0xf) {
    case 0: d->madr = v & 0xffffff; break;
    case 4: d->bcr = v; break;
    case 8: d->active = 0; d->chcr = ch == 6 ? (v & 0x51000000) | 2 : v & 0x71770703; maybe_run(m, ch); break;   // OTC always runs backwards, to RAM
    }
}
