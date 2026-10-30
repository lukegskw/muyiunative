// The CD-ROM decoder and controller (plan.md, phase 2). Public hardware reference: https://psx-spx.consoledev.net/cdromdrive/ (Controller I/O Ports, Control/Seek/Read/
// Status Commands, Mainloop/Responses, Response Timings, Response/Data Queueing); https://psx-spx.consoledev.net/cdromformat/#cdrom-sector-encoding (raw sector layout).
// t_cdread observed the empty-drive implementation rejecting Setloc and returning zero DMA words. Disc-backed ReadN/S now feed eight decoder slots and a separate host
// FIFO: INT1 selects a slot, BFRD locks its bytes, ACK permits the next response. Sector rotation continues independently at 75/150 Hz, including while IRQs wait.
// Mechanical seek/spin-up and partial-sector overwrite timing are not yet measured; their approximations are reported below. CD-DA/XA decoding comes with SPU audio
#include "ps1.h"
#include <string.h>

#define CD (&m->cd)
enum { FIRST = 0xc4e1, FIRST_INIT = 0x13cce, SECOND = 0x4a00 };

static uint32_t period(const ps1 *m) { return CPU_HZ / (CD->mode & 0x80 ? 150 : 75); }
static uint8_t bcd(uint32_t v) { return (uint8_t)(v / 10 * 16 + v % 10); }
static int unbcd(uint8_t v) { return (v & 15) < 10 && (v >> 4) < 10 ? (v >> 4) * 10 + (v & 15) : -1; }
static void msf(uint32_t frames, uint8_t *r) { r[0] = bcd(frames / 4500); r[1] = bcd(frames / 75 % 60); r[2] = bcd(frames % 75); }
static void irq_check(ps1 *m) {
    int on = (CD->hintsts & CD->hintmsk & 0x1f) != 0;
    if (on && !CD->irq) irq_raise(m, IRQ_CDROM);
    CD->irq = (uint8_t)on;
}
static void reply(ps1 *m, uint8_t type, const uint8_t *b, int n, uint32_t delay) {
    CD->now_int = type; memcpy(CD->now_resp, b, (size_t)n); CD->now_n = (uint8_t)n; CD->next = m->cycles + delay;
    sched_update(m);
}
static void reply_later(ps1 *m, uint8_t type, const uint8_t *b, int n, uint32_t delay) {
    CD->later = 1; CD->later_int = type; memcpy(CD->later_resp, b, (size_t)n); CD->later_n = (uint8_t)n; CD->later_delay = delay;
}
static void error(ps1 *m, uint8_t code) { uint8_t r[2] = {(uint8_t)(CD->stat | 1), code}; reply(m, 5, r, 2, FIRST); }
static void stop_read(ps1 *m) { CD->reading = CD->data_pending = CD->end_pending = 0; CD->sector_next = NEVER; }
static void mechanics(ps1 *m) {
    // Response Timings leaves mechanical seek timings unknown. One sector interval represents settling until measured; do not mistake this for cycle accuracy
    RED_ONCE(m, RED_CD_TIMING, "cdrom: mechanical seek/spin-up timing is approximated by one sector interval");
}
static void test(ps1 *m, int np) {
    static const uint8_t version[4] = {0x97, 0x01, 0x10, 0xc2};   // Test 20h: 10 Jan 1997, vC2 (PU-18, US and Europe)
    if (np != 1) { error(m, 0x20); return; }
    switch (CD->param[0]) {
    case 0x04: reply(m, 3, &CD->stat, 1, FIRST); break;
    case 0x05: { static const uint8_t none[2]; reply(m, 3, none, 2, FIRST); break; }   // no wobble-groove SCEx in raw images
    case 0x20: reply(m, 3, version, 4, FIRST); break;
    case 0x22: reply(m, 3, (const uint8_t *)"for U/C", 7, FIRST); break;
    default: { static const uint8_t r[2] = {0x11, 0x10}; reply(m, 5, r, 2, FIRST); break; }
    }
}

static void command(ps1 *m, uint8_t c) {
    uint8_t s = CD->stat; int np = CD->nparam, mounted = m->disc.ntracks != 0;
    CD->busy = 1; CD->nparam = 0;
    int count = c == 2 ? 3 : c == 0x0d ? 2 : c == 0x0e || c == 0x14 ? 1 : 0;
    if (c != 0x19 && np != count) { error(m, 0x20); return; }
    if (!mounted && ((c >= 2 && c <= 9) || (c >= 0x0b && c <= 0x0d) || (c >= 0x10 && c <= 0x16) || c == 0x1b || c == 0x1d)) { error(m, 0x80); return; }
    switch (c) {
    case 0x01: reply(m, 3, &s, 1, FIRST); CD->stat &= (uint8_t)~0x10; break;
    case 0x02: {
        int mm = unbcd(CD->param[0]), ss = unbcd(CD->param[1]), ff = unbcd(CD->param[2]);
        if (mm < 0 || ss < 0 || ss >= 60 || ff < 0 || ff >= 75) { error(m, 0x10); break; }
        // Setloc, Read, Pause: storing a new target must not redirect a read already in flight (covered by t_cdread)
        CD->target = (uint32_t)(mm * 4500 + ss * 75 + ff); CD->setloc = 1; reply(m, 3, &s, 1, FIRST); break;
    }
    case 0x06: case 0x1b:
        reply(m, 3, &s, 1, FIRST);
        if (CD->setloc || !CD->reading) {
            if (CD->setloc) { CD->lba = CD->target >= 150 ? CD->target - 150 : UINT32_MAX; CD->setloc = 0; }
            else CD->lba = CD->have_header ? CD->last_lba : CD->lba;
            CD->have_header = CD->data_pending = CD->end_pending = 0; CD->stat = (s & 0x1f) | 0x42;
            CD->reading = 1; CD->sector_next = m->cycles + FIRST + period(m); mechanics(m); sched_update(m);
        }
        break;
    case 0x07:   // MotorOn; physical spin-up time is still open
        if (s & 2) { error(m, 0x20); break; }   // Control Commands: already spinning is an error, and must not disturb an active read
        mechanics(m); reply(m, 3, &s, 1, FIRST); s = 2; CD->complete_stat = s; reply_later(m, 2, &s, 1, period(m)); break;
    case 0x08: case 0x09: {
        uint32_t delay = c == 8 ? (s & 2 ? (CD->mode & 0x80 ? 0x18a6076 : 0xd38aca) : 0x1d7b) :
            (CD->reading ? (CD->mode & 0x80 ? 0x10bd93 : 0x21181c) : 0x1df2);
        stop_read(m); reply(m, 3, &s, 1, FIRST); s = c == 8 ? 0 : 2; CD->complete_stat = s; reply_later(m, 2, &s, 1, delay); break;
    }
    case 0x0a:
        stop_read(m); CD->mode = 0x20; CD->filter_file = CD->filter_channel = 0; CD->data_n = CD->data_pos = CD->have_sector = CD->have_header = 0;
        reply(m, 3, &s, 1, FIRST_INIT); s = mounted ? 2 : 0; CD->complete_stat = s;
        reply_later(m, 2, &s, 1, mounted ? period(m) : SECOND); if (mounted) mechanics(m); break;
    case 0x0b: case 0x0c: reply(m, 3, &s, 1, FIRST); break;   // muting has no audible effect until CD audio is implemented
    case 0x0d: CD->filter_file = CD->param[0]; CD->filter_channel = CD->param[1]; reply(m, 3, &s, 1, FIRST); break;
    case 0x0e:
        CD->mode = CD->param[0];
        if (CD->mode & 0x5b) RED_ONCE(m, RED_CD_AUDIO, "cdrom: CD-DA/XA, filtering and Setmode bits 1/4 are not modelled yet");
        reply(m, 3, &s, 1, FIRST); break;
    case 0x0f: { uint8_t r[5] = {s, CD->mode, 0, CD->filter_file, CD->filter_channel}; reply(m, 3, r, 5, FIRST); break; }
    case 0x10:
        if (!CD->have_header || CD->stat & 0x40) error(m, 0x80); else reply(m, 3, CD->header, 8, FIRST); break;
    case 0x11: {
        int t = disc_track(m, CD->last_lba); uint8_t r[8] = {0};
        if (t < 0) { error(m, 0x80); break; }
        r[0] = bcd((uint32_t)t + 1); r[1] = CD->last_lba < m->disc.track[t].start ? 0 : 1;
        msf(r[1] ? CD->last_lba - m->disc.track[t].start : m->disc.track[t].start - CD->last_lba, r + 2);
        msf(CD->last_lba + 150, r + 5); reply(m, 3, r, 8, FIRST); break;
    }
    case 0x13: { uint8_t r[3] = {s, 1, bcd((uint32_t)m->disc.ntracks)}; reply(m, 3, r, 3, FIRST); break; }
    case 0x14: {
        int t = unbcd(CD->param[0]); uint8_t r[4] = {s};
        if (t < 0 || t > m->disc.ntracks) { error(m, 0x10); break; }
        msf((t ? m->disc.track[t - 1].start : m->disc.sectors) + 150, r + 1); reply(m, 3, r, 3, FIRST); break;
    }
    case 0x15: case 0x16: {
        stop_read(m); reply(m, 3, &s, 1, FIRST); mechanics(m);
        CD->lba = CD->target >= 150 ? CD->target - 150 : UINT32_MAX; CD->setloc = CD->have_header = 0; CD->stat = 0x42;
        int t = disc_track(m, CD->lba);
        if (t < 0 || (c == 0x15 && m->disc.track[t].audio)) { uint8_t r[2] = {6, 4}; CD->complete_stat = 6; reply_later(m, 5, r, 2, period(m)); }
        else { CD->last_lba = CD->lba; s = 2; CD->complete_stat = s; reply_later(m, 2, &s, 1, period(m)); }
        break;
    }
    case 0x19: test(m, np); break;
    case 0x1a: {
        uint8_t r[8] = {0x08, 0x40}; reply(m, 3, &s, 1, FIRST);
        if (mounted) {
            // GetID: raw BIN has no wobble-groove SCEx, so report unlicensed media instead of inventing a region; authentication metadata remains open
            r[0] = s | 8; r[1] = 0x80; r[2] = m->disc.track[0].mode == 2 ? 0x20 : 0;
            for (int i = 0; i < m->disc.ntracks; i++) if (m->disc.track[i].audio) r[1] |= 0x10;
            RED_ONCE(m, RED_CD_ID, "cdrom: raw disc has no SCEx authentication metadata; GetID reports unlicensed media");
        }
        reply_later(m, 5, r, 8, SECOND); break;
    }
    case 0x1c: stop_read(m); reply(m, 3, &s, 1, FIRST); break;
    case 0x1e:
        stop_read(m); reply(m, 3, &s, 1, FIRST_INIT);
        if (mounted) { s = 2; CD->complete_stat = s; reply_later(m, 2, &s, 1, CPU_HZ); }
        else { uint8_t r[2] = {(uint8_t)(s | 1), 0x80}; reply_later(m, 5, r, 2, SECOND); } break;
    default: RED_ONCE(m, RED_CD, "cdrom: command %02x is not modelled yet", c); error(m, 0x40); break;
    }
}

static void queued(ps1 *m) {
    if (CD->hintsts & 7 || CD->next != NEVER) return;
    if (CD->later) { CD->later = 0; reply(m, CD->later_int, CD->later_resp, CD->later_n, CD->later_delay); }
    else if (CD->pending) { CD->pending = 0; command(m, CD->pending_cmd); }
    else if (CD->data_pending) { uint8_t s = 0x22; reply(m, 1, &s, 1, FIRST); }   // controller mainloop delay leaves time for ACK-before-BFRD
    else if (CD->end_pending) {
        uint8_t r[2] = {CD->stat, 0x80}, type = CD->end_pending; CD->end_pending = 0;
        if (type == 5) r[0] |= 1;
        reply(m, type, r, type == 5 ? 2 : 1, FIRST);
    }
}
static void ack(ps1 *m, uint8_t v) {
    if (v & 0x40) CD->nparam = 0;
    CD->hintsts &= (uint8_t)~(v & 0x1f);
    if (v & 7) CD->nresp = CD->rpos = 0;
    irq_check(m); queued(m);
}
void cd_reset(ps1 *m) {
    memset(CD, 0, sizeof *CD); CD->next = CD->sector_next = NEVER; CD->complete_stat = -1;
    CD->stat = m->disc.ntracks ? 2 : 0; CD->target = 150;
}
void cd_event(ps1 *m) {
    CD->next = NEVER;
    if (CD->complete_stat >= 0 && (CD->now_int == 2 || CD->now_int == 5)) { CD->stat = (uint8_t)CD->complete_stat; CD->complete_stat = -1; }
    if (CD->now_int == 1) { CD->ready = CD->head; CD->have_sector = 1; CD->data_pending = 0; }
    CD->hintsts = (uint8_t)((CD->hintsts & ~7) | CD->now_int);
    memcpy(CD->resp, CD->now_resp, CD->now_n); CD->nresp = CD->now_n; CD->rpos = 0;
    if (!CD->pending) CD->busy = 0;
    irq_check(m);
}
void cd_sector_event(ps1 *m) {
    CD->sector_next += period(m);   // Response Timings: exact rate, independent of controller responses and acknowledgements
    if (CD->lba >= m->disc.sectors) {
        CD->reading = 0; CD->sector_next = NEVER; CD->stat = 2; CD->end_pending = 4; queued(m); return;
    }
    uint8_t next = (CD->head + 1) & 7;
    if (CD->have_sector && next == CD->ready && CD->data_pos == CD->data_n)
        RED_ONCE(m, RED_CD_OVERRUN, "cdrom: sector overrun uses whole decoder slots; partial-sector overwrite timing is not modelled yet");
    if (disc_read(m, CD->lba, CD->sector[next])) {
        RED("cdrom: external sector read failed"); CD->reading = 0; CD->sector_next = NEVER; CD->stat = 2; CD->end_pending = 5; queued(m); return;
    }
    int t = disc_track(m, CD->lba); CD->head = next; CD->last_lba = CD->lba++; CD->stat = 0x22;
    if (t >= 0 && m->disc.track[t].audio) {
        RED_ONCE(m, RED_CD_AUDIO, "cdrom: CD-DA/XA decoding is not modelled yet");
        if (!(CD->mode & 1)) { CD->reading = 0; CD->sector_next = NEVER; CD->stat = 2; CD->end_pending = 5; queued(m); return; }
        CD->have_header = 0;
    } else { memcpy(CD->header, CD->sector[next] + 12, 8); CD->have_header = 1; }
    CD->data_pending = 1;
    if (!(CD->hintsts & 7) && CD->next == NEVER && !CD->pending && !CD->later) {
        CD->now_int = 1; CD->now_n = 1; CD->now_resp[0] = CD->stat; cd_event(m);
    }
}
static uint8_t data_read(ps1 *m) {
    if (CD->data_pos < CD->data_n) return CD->data[CD->data_pos++];
    return CD->data_n ? CD->data[CD->data_n - (CD->data_n == 2048 ? 8 : 4)] : 0;   // RDDATA padding after the FIFO empties
}
uint8_t cd_read(ps1 *m, uint32_t off) {
    switch (off) {
    case 0: return (uint8_t)(CD->index | (CD->nparam == 0) << 3 | (CD->nparam < 16) << 4 | (CD->rpos < CD->nresp) << 5 | (CD->data_pos < CD->data_n) << 6 | CD->busy << 7);
    case 1: return CD->rpos < CD->nresp ? CD->resp[CD->rpos++] : 0;
    case 2: return data_read(m);
    default: return (uint8_t)((CD->index & 1 ? CD->hintsts : CD->hintmsk) | 0xe0);
    }
}
void cd_write(ps1 *m, uint32_t off, uint8_t v) {
    if (off == 0) { CD->index = v & 3; return; }
    switch (off << 2 | CD->index) {
    case 1 << 2 | 0: if (CD->hintsts & 7 || CD->next != NEVER) { CD->pending = 1; CD->pending_cmd = v; CD->busy = 1; } else command(m, v); break;
    case 2 << 2 | 0: if (CD->nparam < 16) CD->param[CD->nparam++] = v; break;
    case 2 << 2 | 1: CD->hintmsk = v & 0x1f; irq_check(m); break;
    case 3 << 2 | 0:
        CD->bfrd = v >> 7;
        if (!(v & 0x80)) CD->data_n = CD->data_pos = 0;
        else if (CD->have_sector && CD->data_pos == CD->data_n) {
            const uint8_t *s = CD->sector[CD->ready]; int off = CD->mode & 0x20 ? 12 : s[15] == 1 ? 16 : 24;
            CD->data_n = CD->mode & 0x20 ? 2340 : 2048; CD->data_pos = 0; memcpy(CD->data, s + off, CD->data_n);
        }
        break;
    case 3 << 2 | 1: ack(m, v); break;
    case 2 << 2 | 2: CD->atv[0] = v; break;
    case 3 << 2 | 2: CD->atv[1] = v; break;
    case 1 << 2 | 3: CD->atv[2] = v; break;
    case 2 << 2 | 3: CD->atv[3] = v; break;
    default: break;   // WRDATA, CI and audio volume application are not implemented
    }
}
uint32_t cd_dma_read(ps1 *m) {
    if (!CD->data_n) RED_ONCE(m, RED_CD, "cdrom: forced DMA read with no sector in the host FIFO");
    uint32_t w = 0; for (int i = 0; i < 4; i++) w |= (uint32_t)data_read(m) << (i * 8); return w;
}
