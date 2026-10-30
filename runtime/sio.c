// SIO0, digital pads and 128KB memory cards (plan.md, phase 2). Clean-room public references: https://psx-spx.consoledev.net/serialinterfacessio/ (DR, SR, MR, CR, BR,
// DR Read/Write Notes, Interrupt Acknowledge Notes); https://psx-spx.consoledev.net/controllersandmemorycards/ (Device addressing, Signals, Communication Sequence,
// Standard Controllers, Memory Card Read/Write Commands, FLAG Byte, Timings, Data Format). t_sio observes the formerly empty port returning FFh without ACK: each byte
// now takes its configured serial clocks, then receives a separate ACK pulse. Cards/pads are external devices retained across reset; all state remains in the machine
#include "ps1.h"
#include <string.h>

#define P (&m->sio)
enum { ACK_DELAY = 400, ACK_WIDTH = 100, CARD_READ_DELAY = 31000 };

static void update(ps1 *m) {
    P->next = P->done;
    if (P->ack_at < P->next) P->next = P->ack_at;
    if (P->ack_end < P->next) P->next = P->ack_end;
    sched_update(m);
}
static void irq_check(ps1 *m) {
    int level = ((P->ctrl & 0x1000) && P->ack) || ((P->ctrl & 0x800) && P->nrx >= (1u << (P->ctrl >> 8 & 3))) || ((P->ctrl & 0x400) && !P->pending);
    if (level && !P->irq) { P->irq = 1; irq_raise(m, IRQ_SIO0); }
}
static void transaction(ps1 *m) { P->device = P->cmd = 0; P->step = 0; P->ack = 0; P->ack_at = P->ack_end = NEVER; }
void sio_reset(ps1 *m) {
    memset(P, 0, offsetof(ps1_sio, pad));   // console reset does not erase a plugged memory card or disconnect a pad
    P->next = P->done = P->ack_at = P->ack_end = NEVER;
}
void sio_set_pad(ps1 *m, int port, int connected, uint16_t pressed) {
    if (port < 0 || port > 1) return;
    P->pad[port] = (ps1_pad){pressed, connected != 0};
    if (!connected && (P->ctrl >> 13 & 1) == port && P->device == 1) { transaction(m); update(m); }
}
int sio_card_insert(ps1 *m, int port, const uint8_t *image, size_t n) {
    if (port < 0 || port > 1 || !image || n != CARD_SIZE) return -1;
    memcpy(P->card[port].data, image, CARD_SIZE); P->card[port].present = 1; P->card[port].flag = 8; P->card[port].dirty = 0;
    if ((P->ctrl >> 13 & 1) == port && P->device == 2) { transaction(m); update(m); }
    return 0;
}
void sio_card_remove(ps1 *m, int port) {
    if (port < 0 || port > 1) return;
    P->card[port].present = 0;
    if ((P->ctrl >> 13 & 1) == port && P->device == 2) { transaction(m); update(m); }
}
void sio_card_format(ps1 *m, int port) {
    if (port < 0 || port > 1) return;
    ps1_card *c = &P->card[port]; memset(c->data, 0xff, CARD_SIZE);
    for (int frame = 0; frame < 36; frame++) {
        uint8_t *s = c->data + frame * CARD_SECTOR; memset(s, 0, CARD_SECTOR);
        if (!frame) { s[0] = 'M'; s[1] = 'C'; }
        else if (frame < 16) { s[0] = 0xa0; s[8] = s[9] = 0xff; }
        else memset(s, 0xff, 4);
        for (int i = 0; i < 127; i++) s[127] ^= s[i];
    }
    memcpy(c->data + 63 * CARD_SECTOR, c->data, CARD_SECTOR); c->present = 1; c->flag = 8; c->dirty = 1;
    if ((P->ctrl >> 13 & 1) == port && P->device == 2) { transaction(m); update(m); }
}

static uint8_t exchange(ps1 *m, uint8_t v, uint32_t *ack_delay) {
    int port = P->tx_port; uint16_t step = P->step++; uint8_t r = 0xff; P->want_ack = 1;
    if (!step) {
        P->device = v == 1 && P->pad[port].present ? 1 : v == 0x81 && P->card[port].present ? 2 : 3;
        P->want_ack = P->device != 3; return r;
    }
    if (P->device == 1 && P->pad[port].present) {
        switch (step) {
        case 1: r = 0x41; P->buttons = (uint16_t)(~P->pad[port].pressed | 6); if (v != 0x42) P->want_ack = 0; break;
        case 2: r = 0x5a; break;
        case 3: r = (uint8_t)P->buttons; break;
        case 4: r = (uint8_t)(P->buttons >> 8); P->want_ack = 0; break;
        default: P->want_ack = 0; break;
        }
    } else if (P->device == 2 && P->card[port].present) {
        ps1_card *c = &P->card[port];
        if (step == 1) { P->cmd = v; r = c->flag; if (v != 0x52 && v != 0x57 && v != 0x53) P->want_ack = 0; }
        else if (step == 2) r = 0x5a;
        else if (step == 3) r = 0x5d;
        else if (P->cmd == 0x53) {
            static const uint8_t id[] = {0x5c, 0x5d, 4, 0, 0, 0x80}; r = id[step - 4]; if (step == 9) P->want_ack = 0;
        } else if (step == 4) { P->addr = (uint16_t)v << 8; P->checksum = v; P->prev = v; r = 0; }
        else if (step == 5) { P->addr |= v; P->checksum ^= v; r = P->prev; P->prev = v; }
        else if (P->cmd == 0x52) {
            if (step == 6) { r = 0x5c; *ack_delay += CARD_READ_DELAY; }   // Memory Card Timings: late ACK after byte seven, not a delayed RX byte
            else if (step == 7) r = 0x5d;
            else if (step == 8) r = P->addr < 1024 ? (uint8_t)(P->addr >> 8) : 0xff;
            else if (step == 9) { r = P->addr < 1024 ? (uint8_t)P->addr : 0xff; if (P->addr >= 1024) P->want_ack = 0; }
            else if (step < 138) { r = c->data[P->addr * CARD_SECTOR + step - 10]; P->checksum ^= r; }
            else if (step == 138) r = P->checksum;
            else { r = 0x47; P->want_ack = 0; }
        } else if (P->cmd == 0x57) {
            if (step < 134) { r = P->prev; P->prev = v; P->write[step - 6] = v; P->checksum ^= v; }
            else if (step == 134) {
                r = P->prev; P->result = P->addr >= 1024 ? 0xff : v != P->checksum ? 0x4e : 0x47;
                if (P->result == 0x47) { memcpy(c->data + P->addr * CARD_SECTOR, P->write, CARD_SECTOR); c->flag &= (uint8_t)~8u; c->dirty = 1; }
                else if (!(P->reported & 4)) { P->reported |= 4; RED("sio: memory-card write-error FLAG bit timing is not modelled yet"); }
            } else if (step == 135) r = 0x5c;
            else if (step == 136) r = 0x5d;
            else { r = P->result; P->want_ack = 0; }
        }
    } else P->want_ack = 0;
    if (!P->want_ack) P->device = 3;
    return r;
}

static uint32_t bit_cycles(const ps1 *m) {
    unsigned f = P->mode & 3, mul = f < 2 ? 1 : f == 2 ? 16 : 64, period = P->baud * mul & ~1u;
    return period ? period : 1;   // BR: serial clock consists of two half-periods; odd reload values round down
}
static void start(ps1 *m, uint64_t at) {
    if (P->active || !P->pending || !((P->ctrl & 1) || P->latched)) return;
    P->tx = P->hold; P->pending = P->latched = 0; P->active = 1;
    P->tx_port = P->ctrl >> 13 & 1; P->selected = P->ctrl >> 1 & 1;
    unsigned bits = 5 + (P->mode >> 2 & 3) + (P->mode >> 4 & 1);
    P->done = at + (uint64_t)bit_cycles(m) * bits;
}
static void receive(ps1 *m, uint8_t v) {
    if (P->nrx == 8) P->rx[(P->rpos + 7) & 7] = v;   // DR Read Notes: overwrite newest slot on overrun, retaining the oldest seven
    else { P->rx[(P->rpos + P->nrx) & 7] = v; P->nrx++; }
}
void sio_event(ps1 *m) {
    uint64_t at = P->next;
    if (P->ack_end == at) { P->ack = 0; P->ack_end = NEVER; }
    if (P->ack_at == at) { P->ack = 1; P->ack_at = NEVER; P->ack_end = at + ACK_WIDTH; }
    if (P->done == at) {
        P->active = 0; P->done = NEVER; P->want_ack = 0; uint8_t r = 0xff; uint32_t delay = ACK_DELAY;
        int selected = P->selected && (P->ctrl & 2) && P->tx_port == (P->ctrl >> 13 & 1);
        if (selected && (P->mode & 0x3c) == 0x0c) r = exchange(m, P->tx, &delay);
        else if (selected && !(P->reported & 1)) { P->reported |= 1; RED("sio: peripheral framing other than 8 bits without parity is not modelled yet"); }
        if (selected || P->ctrl & 4) { receive(m, r); P->ctrl &= (uint16_t)~4u; }
        if (P->want_ack) P->ack_at = at + delay;
        start(m, at);
    }
    irq_check(m); update(m);
}
static uint8_t peek(const ps1 *m, unsigned i) { return i < P->nrx ? P->rx[(P->rpos + i) & 7] : 0; }
static void pop(ps1 *m, unsigned n) { while (n-- && P->nrx) { P->rpos = (P->rpos + 1) & 7; P->nrx--; } }
uint32_t sio_read(ps1 *m, uint32_t off, unsigned n) {
    if (off == 0) { uint32_t v = 0; for (unsigned i = 0; i < n; i++) v |= (uint32_t)peek(m, i) << (i * 8); pop(m, n == 4 ? 4 : 1); return v; }
    uint32_t value = 0;
    if (off >= 4 && off < 8) {
        uint32_t half = bit_cycles(m) / 2; if (!half) half = 1;
        uint32_t timer = half - (uint32_t)((m->cycles - P->baud_at) % half);
        value = (uint32_t)!P->pending | (uint32_t)(P->nrx != 0) << 1 | (uint32_t)(!P->active && !P->pending) << 2 | (uint32_t)P->ack << 7 | (uint32_t)P->irq << 9 | timer << 11;
        return value >> ((off - 4) * 8);
    }
    if (off >= 8 && off < 12) value = (uint32_t)P->mode | (uint32_t)P->ctrl << 16;
    else if (off >= 14 && off < 16) return P->baud >> ((off - 14) * 8);
    else if (off == 0x14) return 5;   // unconnected SIO1
    return value >> ((off & 3) * 8);
}
void sio_write(ps1 *m, uint32_t off, uint32_t v, unsigned n) {
    if (!off) { P->hold = (uint8_t)v; P->pending = 1; P->latched = P->ctrl & 1; start(m, m->cycles); irq_check(m); update(m); return; }
    uint16_t mode = P->mode, ctrl = P->ctrl, baud = P->baud; int mode_write = 0, ctrl_write = 0, baud_write = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t a = off + i; unsigned shift = (a & 1) * 8; uint16_t b = (uint16_t)(v >> (i * 8) & 0xff) << shift, keep = (uint16_t)~(0xffu << shift);
        if (a == 8 || a == 9) { mode = (mode & keep) | b; mode_write = 1; }
        if (a == 10 || a == 11) { ctrl = (ctrl & keep) | b; ctrl_write = 1; }
        if (a == 14 || a == 15) { baud = (baud & keep) | b; baud_write = 1; }
    }
    if (mode_write) P->mode = mode & 0x013f;
    if (baud_write) { P->baud = baud; P->baud_at = m->cycles; }
    if (ctrl_write) {
        if (ctrl & 0x40) { sio_reset(m); update(m); return; }
        if ((ctrl ^ P->ctrl) & 0x2002) { transaction(m); P->selected = 0; }
        // Controller Overview, DSR interrupt: ACK low holds SR.9 set, so ERRRST cannot create another I_STAT edge until the pulse ends
        if ((ctrl & 0x10) && !(P->ack && (ctrl & 0x1000))) P->irq = 0;
        P->ctrl = ctrl & 0x3f07; start(m, m->cycles);
    }
    if (off >= 0x10 && !(P->reported & 2)) { P->reported |= 2; RED("sio: external SIO1 serial link is not modelled yet"); }
    irq_check(m); update(m);
}
