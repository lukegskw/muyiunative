// The CD-ROM drive (psx-spx, CDROM Drive): the decoder's four register banks, the parameter and response FIFOs, and the interrupt types INT1..INT5 the drive's controller
// answers with. There is no disc yet (phase 2): commands answer as an empty drive with its lid closed does (Status code: error 80h for the commands that need a disc;
// GetID's "No Disk" response), timed like the drive's measured responses (Response Timings). A command sent while an interrupt is unacknowledged waits for it, and a
// second response (INT2, or INT5) follows only once the first is acknowledged (Mainloop/Responses)
#include "ps1.h"
#include <string.h>

#define CD (&m->cd)
enum { FIRST = 0xc4e1, FIRST_INIT = 0x13cce, SECOND = 0x4a00 };   // cycles: average first response, Init's first response, GetID's second response

static void irq_check(ps1 *m) {   // IRQ 2 when an enabled flag rises
    int on = (CD->hintsts & CD->hintmsk & 0x1f) != 0;
    if (on && !CD->irq) irq_raise(m, IRQ_CDROM);
    CD->irq = (uint8_t)on;
}

static void reply(ps1 *m, uint8_t type, const uint8_t *b, int n, uint32_t delay) {
    CD->now_int = type; memcpy(CD->now_resp, b, (size_t)n); CD->now_n = (uint8_t)n; CD->next = m->cycles + delay;
    sched_update(m);
}

static void reply_later(ps1 *m, uint8_t type, const uint8_t *b, int n) {
    CD->later = 1; CD->later_int = type; memcpy(CD->later_resp, b, (size_t)n); CD->later_n = (uint8_t)n; CD->later_delay = SECOND;
}

static void error(ps1 *m, uint8_t code) { uint8_t r[2] = {(uint8_t)(CD->stat | 1), code}; reply(m, 5, r, 2, FIRST); }

static void test(ps1 *m, int np) {   // command 19h by its sub-function
    static const uint8_t version[4] = {0x97, 0x01, 0x10, 0xc2};   // the controller BIOS of 10 Jan 1997, vC2 (PU-18, US and Europe)
    if (!np) { error(m, 0x20); return; }
    switch (CD->param[0]) {
    case 0x04: reply(m, 3, &CD->stat, 1, FIRST); break;
    case 0x05: { static const uint8_t none[2]; reply(m, 3, none, 2, FIRST); break; }   // SCEx counters: nothing read
    case 0x20: reply(m, 3, version, 4, FIRST); break;
    case 0x22: reply(m, 3, (const uint8_t *)"for U/C", 7, FIRST); break;
    default: { static const uint8_t r[2] = {0x11, 0x10}; reply(m, 5, r, 2, FIRST); break; }
    }
}

static void command(ps1 *m, uint8_t c) {
    uint8_t s = CD->stat; int np = CD->nparam;
    CD->busy = 1; CD->nparam = 0;
    switch (c) {
    case 0x01: reply(m, 3, &s, 1, FIRST); CD->stat &= (uint8_t)~0x10; break;   // Nop also clears the shell-open flag
    case 0x0a: CD->mode = 0x20; reply(m, 3, &s, 1, FIRST_INIT); reply_later(m, 2, &s, 1); break;
    case 0x0e: if (np != 1) { error(m, 0x20); break; } CD->mode = CD->param[0]; reply(m, 3, &s, 1, FIRST); break;
    case 0x0f: { uint8_t r[5] = {s, CD->mode}; reply(m, 3, r, 5, FIRST); break; }
    case 0x19: test(m, np); break;
    case 0x1a: { static const uint8_t none[8] = {0x08, 0x40}; reply(m, 3, &s, 1, FIRST); reply_later(m, 5, none, 8); break; }
    case 0x1c: reply(m, 3, &s, 1, FIRST); break;
    case 0x1e: { uint8_t r[2] = {(uint8_t)(s | 1), 0x80}; reply(m, 3, &s, 1, FIRST_INIT); reply_later(m, 5, r, 2); break; }   // ReadTOC: refused late (unconfirmed)
    case 0x02 ... 0x09: case 0x0b ... 0x0d: case 0x10 ... 0x16: case 0x1b: case 0x1d: error(m, 0x80); break;   // they need a disc
    default: error(m, 0x40); break;
    }
}

static void ack(ps1 *m, uint8_t v) {   // HCLRCTL
    if (v & 0x40) CD->nparam = 0;
    CD->hintsts &= (uint8_t)~(v & 0x1f);
    if (v & 7) CD->nresp = CD->rpos = 0;
    irq_check(m);
    if (CD->hintsts & 7 || CD->next != NEVER) return;
    if (CD->later) { CD->later = 0; reply(m, CD->later_int, CD->later_resp, CD->later_n, CD->later_delay); }
    else if (CD->pending) { CD->pending = 0; command(m, CD->pending_cmd); }
}

void cd_reset(ps1 *m) { memset(CD, 0, sizeof *CD); CD->next = NEVER; }

void cd_event(ps1 *m) {   // the response arrives
    CD->next = NEVER;
    CD->hintsts = (uint8_t)((CD->hintsts & ~7) | CD->now_int);
    memcpy(CD->resp, CD->now_resp, CD->now_n); CD->nresp = CD->now_n; CD->rpos = 0;
    if (!CD->pending) CD->busy = 0;
    irq_check(m);
}

uint8_t cd_read(ps1 *m, uint32_t off) {
    switch (off) {
    case 0: return (uint8_t)(CD->index | (CD->nparam == 0) << 3 | (CD->nparam < 16) << 4 | (CD->rpos < CD->nresp) << 5 | CD->busy << 7);   // HSTS
    case 1: return CD->rpos < CD->nresp ? CD->resp[CD->rpos++] : 0;
    case 2: return 0;   // RDDATA: no sector
    default: return (uint8_t)((CD->index & 1 ? CD->hintsts : CD->hintmsk) | 0xe0);
    }
}

void cd_write(ps1 *m, uint32_t off, uint8_t v) {
    if (off == 0) { CD->index = v & 3; return; }
    switch (off << 2 | CD->index) {   // register, then bank
    case 1 << 2 | 0: if (CD->hintsts & 7 || CD->next != NEVER) { CD->pending = 1; CD->pending_cmd = v; CD->busy = 1; } else command(m, v); break;
    case 2 << 2 | 0: if (CD->nparam < 16) CD->param[CD->nparam++] = v; break;
    case 2 << 2 | 1: CD->hintmsk = v & 0x1f; irq_check(m); break;
    case 3 << 2 | 1: ack(m, v); break;
    case 2 << 2 | 2: CD->atv[0] = v; break;
    case 3 << 2 | 2: CD->atv[1] = v; break;
    case 1 << 2 | 3: CD->atv[2] = v; break;
    case 2 << 2 | 3: CD->atv[3] = v; break;
    default: break;   // HCHPCTL (no sector to request), WRDATA and CI (the sound map), ADPCTL: no effect without a disc
    }
}

uint32_t cd_dma_read(ps1 *m) { RED_ONCE(m, RED_CD, "cdrom: DMA read with no sector in the buffer"); return 0; }
