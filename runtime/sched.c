// The scheduler (plan.md, section 2: a scheduler that counts CPU cycles): the CPU runs until the next device event (a scanline, a timer reaching a value that raises its
// IRQ, a CD-ROM response), the event runs, and so on. Between events, a device's state is brought up to date when the CPU reads it
#include "ps1.h"

void sched_update(ps1 *m) {
    uint64_t e = m->gpu.next_line < m->gpu.next_hblank ? m->gpu.next_line : m->gpu.next_hblank;
    for (int n = 0; n < 3; n++) if (m->tmr[n].next < e) e = m->tmr[n].next;
    if (m->dma_next < e) e = m->dma_next;
    if (m->cd.next < e) e = m->cd.next;
    if (m->cd.sector_next < e) e = m->cd.sector_next;
    if (m->sio.next < e) e = m->sio.next;
    if (m->spu.next < e) e = m->spu.next;
    m->next_event = e;
}

void sched_run(ps1 *m) {
    while (m->dma_next <= m->cycles) dma_event(m);
    for (;;) {   // scanlines and hblank edges, in their order
        uint64_t l = m->gpu.next_line, h = m->gpu.next_hblank;
        if (l <= h && l <= m->cycles) gpu_line(m);
        else if (h < l && h <= m->cycles) gpu_hblank(m);
        else break;
    }
    for (int n = 0; n < 3; n++) if (m->tmr[n].next <= m->cycles) tmr_event(m, n);
    if (m->cd.next <= m->cycles) cd_event(m);
    while (m->cd.sector_next <= m->cycles) cd_sector_event(m);
    while (m->sio.next <= m->cycles) sio_event(m);
    while (m->spu.next <= m->cycles) spu_event(m);
    sched_update(m);
}
