// The controller and memory card port, SIO0 (psx-spx, Controllers and Memory Cards; Serial Interfaces): nothing is plugged in yet (phase 2). A byte sent takes eight
// periods of the baud timer, and the answer is FFh with no /ACK, so the BIOS and games find the slots empty and IRQ 7 never comes. SIO1 has nothing connected either
#include "ps1.h"

#define P (&m->sio)

void sio_reset(ps1 *m) { *P = (ps1_sio){0}; }

uint32_t sio_read(ps1 *m, uint32_t off) {
    int done = m->cycles >= P->done;
    switch (off) {
    case 0x00: if (P->rx_full && done) P->rx_full = 0; return 0xff;                                    // JOY_RX_DATA
    case 0x04: return 1u | (uint32_t)(P->rx_full && done) << 1 | (uint32_t)done << 2;              // JOY_STAT: TX ready, RX not empty, TX done
    case 0x08: return P->mode;
    case 0x0a: return P->ctrl;
    case 0x0e: return P->baud;
    case 0x14: return 5;                                                                              // SIO1 status: TX ready and done
    default: return 0;
    }
}

void sio_write(ps1 *m, uint32_t off, uint32_t v) {
    switch (off) {
    case 0x00: P->rx_full = 1; P->done = m->cycles + (uint64_t)(P->baud ? P->baud : 0x88) * 8; break;
    case 0x08: P->mode = (uint16_t)v; break;
    case 0x0a: if (v & 0x40) sio_reset(m); else P->ctrl = (uint16_t)(v & ~0x50u); break;               // bit 6 resets the port; bit 4 acknowledges
    case 0x0e: P->baud = (uint16_t)v; break;
    default: break;
    }
}
