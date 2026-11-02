// The DMA controller (psx-spx, DMA Channels): channels 0..6 (MDEC in, MDEC out, GPU, CD-ROM, SPU, PIO, OTC) move words between main RAM and the devices; DPCR enables
// them, DICR collects their completions into IRQ 3. Sync mode 0 moves a block at once, mode 1 a number of blocks, mode 2 (GPU only) follows a linked list of packets.
// GPU linked lists run a packet per scheduled event. CPU bus stalls during packets are not modelled: psx-spx, CPU Operation during DMA, stalls RAM/I/O reads and a full
// write queue during transfer, releasing the CPU between entries. Other transfers run whole when started; MDEC's channels alternate as their FIFO requests allow.
// Synchronous transfers charge the CPU a cycle per word; linked lists currently let the CPU run during those word cycles too (device waits remain open).
// PIO has nothing connected. Public reference: https://psx-spx.consoledev.net/dmachannels/ and Macroblock Decoder, DMA
#include "ps1.h"
#include <string.h>

void dma_reset(ps1 *m) { memset(m->dma, 0, sizeof m->dma); m->dpcr = 0x07654321; m->dicr = 0; m->dma_next = NEVER; }

static uint32_t *ram32(ps1 *m, uint32_t a) { return (uint32_t *)(m->ram + (a & 0x1ffffc)); }

static void dicr_update(ps1 *m) {   // bit 31 is the master flag; IRQ 3 is raised when it rises
    uint32_t d = m->dicr, was = d >> 31;
    int on = d & 0x8000 || (d & 0x800000 && d & 0x7f000000);
    m->dicr = (d & 0x7fffffff) | (uint32_t)on << 31;
    if (on && !was) irq_raise(m, IRQ_DMA);
}

static void complete(ps1 *m, int ch) {
    m->dma[ch].active = m->dma[ch].gpu_wait = 0; m->dma[ch].chcr &= ~0x11000000u;
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
        else { int32_t offset = mdec_dma_offset(m); *ram32(m, d->addr + (uint32_t)offset) = mdec_dma_read(m); if (m->native_active) native_written(m, (d->addr + (uint32_t)offset) & 0x1ffffc, 4); }
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
    if (mode > 1 || (d->chcr & 1) != (uint32_t)!ch) { RED_ONCE(m, RED_DMA_MDEC, "dma: unsupported MDEC channel direction or sync mode"); return; }
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
    else RED_ONCE(m, RED_DMA_TO_DEVICE, "dma: channel %d towards its device: nothing takes the words (PIO has nothing connected, the CD-ROM only sends)", ch);
}

static uint32_t from_device(ps1 *m, int ch) {
    if (ch == 2) return gpu_read(m);
    if (ch == 3) return cd_dma_read(m);
    if (ch == 4) return spu_dma_read(m);
    RED_ONCE(m, RED_DMA_FROM_DEVICE, "dma: channel %d from its device: nothing sends, RAM gets 0 (PIO has nothing connected)", ch);
    return 0;
}

// The phase-3 route spent 20 seconds inside one DMA store, drawing a three-packet loop 350,000 times with no guest time passing. This must not be a C loop:
// psx-spx, CPU Operation during DMA, releases the CPU after each linked-list entry. Read each header when its event arrives, so the CPU can change or stop the list.
// Transfer Rates gives one cycle per GPU word. Mode 2's request is GPUSTAT.28 (GP1 DMA Direction): a completed drawing command holds the next packet until ready.
// A packet is still transferred whole; FIFO/bus timing and CPU bus stalls within a packet remain open. No packet limit invents a completion or drops data.
// t_dma's first GPU list hid every later DMA gap through RED_DMA; each independent gap now owns its report bit, including MDEC and both PIO directions.
static void list_begin(ps1 *m) {
    ps1_dma *d = &m->dma[2];
    if (!d->active) { d->addr = d->madr & 0x1ffffc; d->active = 1; d->gpu_wait = 0; d->chcr &= ~0x10000000u; m->dma_next = m->cycles + 1; }
    RED_ONCE(m, RED_DMA_LIST, "dma: GPU lists do not model FIFO/bus timing or CPU bus stalls within packets");
    if (!d->left && m->gpu.dma_dir != 1 && m->gpu.dma_dir != 2) m->dma_next = NEVER;
    sched_update(m);
}

void dma_gpu_resume(ps1 *m) {
    ps1_dma *d = &m->dma[2];
    if (!d->active || !(m->dpcr & 0x800) || d->left) return;
    if (m->gpu.dma_dir != 1 && m->gpu.dma_dir != 2) { m->dma_next = NEVER; d->gpu_wait = 0; }
    else if (m->dma_next == NEVER || d->gpu_wait) {
        m->dma_next = m->cycles + 1; d->gpu_wait = 0;
        if (m->gpu.dma_dir == 2 && m->gpu.draw_until > m->dma_next) { m->dma_next = m->gpu.draw_until; d->gpu_wait = 1; }
    }
    sched_update(m);
}

void dma_event(ps1 *m) {
    ps1_dma *d = &m->dma[2];
    if (!d->active || !(m->dpcr & 0x800)) { m->dma_next = NEVER; return; }
    if (d->left) { complete(m, 2); m->dma_next = NEVER; return; }
    d->gpu_wait = 0;
    if (m->gpu.dma_dir != 1 && m->gpu.dma_dir != 2) { m->dma_next = NEVER; return; }
    if (m->gpu.dma_dir == 2 && m->dma_next < m->gpu.draw_until) { m->dma_next = m->gpu.draw_until; d->gpu_wait = 1; return; }
    uint32_t h = *ram32(m, d->addr), n = h >> 24;
    for (uint32_t i = 1; i <= n; i++) gpu_gp0_at(m, *ram32(m, d->addr + 4 * i), m->dma_next + i);
    d->madr = h & 0xffffff; d->addr = h & 0x1ffffc;
    d->left = (h & 0x800000) != 0;   // finish only after the final packet's words have taken their time
    m->dma_next += n + 1;
}

static void run(ps1 *m, int ch) {
    ps1_dma *d = &m->dma[ch];
    uint32_t mode = d->chcr >> 9 & 3, addr = d->madr & 0x1ffffc, step = d->chcr & 2 ? (uint32_t)-4 : 4, words = 0;
    if (ch == 6) {   // an empty ordering table: each entry points to the one before it, the first holds the end code (psx-spx, GPU Depth Ordering)
        uint32_t n = d->bcr & 0xffff ? d->bcr & 0xffff : 0x10000;
        for (uint32_t i = 0; i < n; i++, addr -= 4) { *ram32(m, addr) = i == n - 1 ? 0xffffff : (addr - 4) & 0x1fffff; if (m->native_active) native_written(m, addr & 0x1ffffc, 4); }
        words = n;
    } else if (mode >= 2) { RED_ONCE(m, RED_DMA_MODE, "dma: linked-list mode is only supported on the GPU channel"); return;
    } else {
        uint32_t n = mode == 0 ? (d->bcr & 0xffff ? d->bcr & 0xffff : 0x10000) : (d->bcr & 0xffff) * (d->bcr >> 16);
        for (uint32_t i = 0; i < n; i++, addr += step) { if (d->chcr & 1) to_device(m, ch, *ram32(m, addr)); else { *ram32(m, addr) = from_device(m, ch); if (m->native_active) native_written(m, addr & 0x1ffffc, 4); } }
        words = n;
        if (mode == 1) { d->madr = addr & 0xffffff; d->bcr &= 0xffff; }
    }
    m->cycles += words;
    complete(m, ch);
}

static void maybe_run(ps1 *m, int ch) {   // started by CHCR bit 24, and in sync mode 0 also by the trigger bit 28, on a channel DPCR enables
    uint32_t c = m->dma[ch].chcr;
    if (c & 0x01000000 && (c >> 9 & 3 || c & 0x10000000 || m->dma[ch].active) && m->dpcr >> (4 * ch + 3) & 1) {
        if (ch < 2) mdec_begin(m, ch); else if (ch == 2 && (c >> 9 & 3) == 2) list_begin(m); else run(m, ch);
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
    if (off == 0x70) { m->dpcr = v; if (!(v & 0x800)) m->dma_next = NEVER; else if (m->dma[2].active && m->dma_next == NEVER) m->dma_next = m->cycles + 1; for (int ch = 0; ch < 7; ch++) maybe_run(m, ch); sched_update(m); return; }
    if (off == 0x74) {   // t_dma: keep the old master output until recomputed, so writes that leave it high cannot retrigger IRQ3; a 1 acknowledges each flag
        m->dicr = (m->dicr & 0x80000000) | (m->dicr & 0x7f000000 & ~v) | (v & 0x00ff803f); dicr_update(m); return;
    }
    int ch = off >> 4;
    if (ch > 6) return;
    ps1_dma *d = &m->dma[ch];
    switch (off & 0xf) {
    case 0: d->madr = v & 0xffffff; break;
    case 4: d->bcr = v; break;
    case 8: d->active = d->gpu_wait = 0; d->left = 0; if (ch == 2) m->dma_next = NEVER; d->chcr = ch == 6 ? (v & 0x51000000) | 2 : v & 0x71770703; maybe_run(m, ch); sched_update(m); break;   // OTC always runs backwards, to RAM
    }
}
