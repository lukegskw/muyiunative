// The scheduler (plan.md, section 2: a scheduler that counts CPU cycles): the CPU runs until the next device event (a scanline, a timer reaching a value that raises its
// IRQ, a CD-ROM response), the event runs, and so on. Between events, a device's state is brought up to date when the CPU reads it
#include "ps1.h"

void sched_update(ps1 *m) {
    uint64_t e = m->gpu.next_line;
    for (int n = 0; n < 3; n++) if (m->tmr[n].next < e) e = m->tmr[n].next;
    if (m->cd.next < e) e = m->cd.next;
    m->next_event = e;
}

void sched_run(ps1 *m) {
    while (m->gpu.next_line <= m->cycles) gpu_line(m);
    for (int n = 0; n < 3; n++) if (m->tmr[n].next <= m->cycles) tmr_event(m, n);
    if (m->cd.next <= m->cycles) cd_event(m);
    sched_update(m);
}
