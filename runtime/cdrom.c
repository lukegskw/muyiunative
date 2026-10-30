// The CD-ROM decoder and controller (plan.md, phase 2). Public hardware reference: https://psx-spx.consoledev.net/cdromdrive/ (Controller I/O Ports, Control/Seek/Read/
// Status/CD Audio Commands, Mainloop/Responses, Response Timings, Response/Data Queueing); https://psx-spx.consoledev.net/cdromformat/ (sector layout, XA subheader,
// XA-ADPCM and its zigzag interpolation, the system area's licence string). ReadN/S feed eight decoder slots and a separate host FIFO: INT1 selects a slot, BFRD locks
// its bytes, ACK permits the next response; sectors keep arriving at 75/150 Hz while IRQs wait. XA-ADPCM sectors and CD-DA (Play) reach the SPU through a 44.1 kHz
// buffer (cd_audio). Mechanical seek/spin-up and partial-sector overwrite timing are not measured; their approximations are reported below
#include "ps1.h"
#include <string.h>

#define CD (&m->cd)
enum { FIRST = 0xc4e1, FIRST_INIT = 0x13cce, SECOND = 0x4a00 };

static uint32_t period(const ps1 *m) { return CPU_HZ / (CD->mode & 0x80 ? 150 : 75); }
static uint8_t bcd(uint32_t v) { return (uint8_t)(v / 10 * 16 + v % 10); }
static int unbcd(uint8_t v) { return (v & 15) < 10 && (v >> 4) < 10 ? (v >> 4) * 10 + (v & 15) : -1; }
static void msf(uint32_t frames, uint8_t *r) { r[0] = bcd(frames / 4500); r[1] = bcd(frames / 75 % 60); r[2] = bcd(frames % 75); }
static int16_t sat(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
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
static void stop_read(ps1 *m) { CD->reading = CD->playing = CD->data_pending = CD->end_pending = CD->report_n = 0; CD->sector_next = NEVER; }
static int idle(const ps1 *m) { return !(CD->hintsts & 7) && CD->next == NEVER && !CD->pending && !CD->later; }
static uint8_t read_stat(const ps1 *m) { return (uint8_t)(CD->stat | (CD->mode & 0x10) >> 1); }   // Setmode bit 4 also sets IdError in INT1's status
static void mechanics(ps1 *m) {
    // Response Timings leaves mechanical seek timings unknown. One sector interval represents settling until measured; do not mistake this for cycle accuracy
    RED_ONCE(m, RED_CD_TIMING, "cdrom: mechanical seek/spin-up timing is approximated by one sector interval");
}

// The licence string in sector 4 names the region the disc was pressed for (CDROM ISO Volume Descriptors, System Area). The drive checks the SCEx string in the wobble
// groove instead, which no BIN holds, so this drive takes the licence for it: a licensed disc reads as on a console of its region (plan.md, decision log)
static uint8_t licence(ps1 *m) {
    uint8_t s[DISC_SECTOR];
    if (!m->disc.ntracks || m->disc.track[0].audio || disc_read(m, 4, s)) return 0;
    const char *text = (const char *)s + (s[15] == 2 ? 24 : 16) + 0x20;
    if (memcmp(text, "Sony Computer Entertainment ", 28)) return 0;
    return !memcmp(text + 28, "Amer", 4) ? 'A' : !memcmp(text + 28, "Euro", 4) ? 'E' : !memcmp(text + 28, "Inc.", 4) ? 'I' : 0;
}

static void test(ps1 *m, int np) {
    // Test 20h: the PU-18 controller of the licence's region, 10 Jan 1997 (US and Europe) or 12 Sep 1996 (Japan), both vC2; Test 22h names its region
    static const uint8_t version[4] = {0x97, 0x01, 0x10, 0xc2}, japan[4] = {0x96, 0x09, 0x12, 0xc2};
    if (np != 1) { error(m, 0x20); return; }
    switch (CD->param[0]) {
    case 0x04: CD->scex = 0; reply(m, 3, &CD->stat, 1, FIRST); break;   // counting restarts where the head is, and the SCEx string is only in the lead-in
    case 0x05: { uint8_t r[2] = {CD->scex, CD->scex}; reply(m, 3, r, 2, FIRST); break; }
    case 0x20: reply(m, 3, CD->region == 'I' ? japan : version, 4, FIRST); break;
    case 0x22: {
        const char *r = CD->region == 'E' ? "for Europe" : CD->region == 'I' ? "for Japan" : "for U/C";
        reply(m, 3, (const uint8_t *)r, (int)strlen(r), FIRST); break;
    }
    default: { static const uint8_t r[2] = {0x11, 0x10}; reply(m, 5, r, 2, FIRST); break; }
    }
}

static void command(ps1 *m, uint8_t c) {
    uint8_t s = CD->stat; int np = CD->nparam, mounted = m->disc.ntracks != 0;
    CD->busy = 1;
    // Command Summary: 00h, 17h-18h and 1Fh-FFh answer INT5 with error 40h whatever their parameters; an unsupported 1Fh (VideoCD) leaves those in the FIFO
    if (!c || c == 0x17 || c == 0x18 || c >= 0x1f) {
        if (c >= 0x58 && c <= 0x5f) RED_ONCE(m, RED_CD, "cdrom: command %02x crashes the console's drive controller; here it answers as an invalid command", c);
        else if (c >= 0x50 && c <= 0x57) RED_ONCE(m, RED_CD_UNLOCK, "cdrom: the secret unlock commands (50h-57h) answer as on the console, but unlock nothing");
        if (c != 0x1f) CD->nparam = 0;
        error(m, 0x40); return;
    }
    CD->nparam = 0;
    // Reset takes any number of parameters and Play an optional track (Control Commands, CD Audio Commands); Test checks its own
    int count = c == 2 ? 3 : c == 0x0d || c == 0x1d ? 2 : c == 0x0e || c == 0x12 || c == 0x14 ? 1 : 0;
    if (c != 0x19 && c != 0x1c && !(c == 3 && np <= 1) && np != count) { error(m, 0x20); return; }
    if (!mounted && ((c >= 2 && c <= 9) || (c >= 0x0b && c <= 0x0d) || (c >= 0x10 && c <= 0x16) || c == 0x1b || c == 0x1d)) { error(m, 0x80); return; }
    switch (c) {
    case 0x01: reply(m, 3, &s, 1, FIRST); CD->stat &= (uint8_t)~0x10; break;
    case 0x02: {
        int mm = unbcd(CD->param[0]), ss = unbcd(CD->param[1]), ff = unbcd(CD->param[2]);
        if (mm < 0 || ss < 0 || ss >= 60 || ff < 0 || ff >= 75) { error(m, 0x10); break; }
        // Setloc, Read, Pause: storing a new target must not redirect a read already in flight (covered by t_cdread)
        CD->target = (uint32_t)(mm * 4500 + ss * 75 + ff); CD->setloc = 1; reply(m, 3, &s, 1, FIRST); break;
    }
    case 0x03: {   // Play: track 1..N starts that track, a higher one restarts the current track, none (or 0) plays from the Setloc target or where the head is
        int t = np ? unbcd(CD->param[0]) : 0, cur = disc_track(m, CD->last_lba);
        reply(m, 3, &s, 1, FIRST);
        if (CD->playing && !t && !CD->setloc) break;
        uint32_t at = CD->lba;
        if (t >= 1 && t <= m->disc.ntracks) at = m->disc.track[t - 1].start;
        else if (t) at = m->disc.track[cur < 0 ? 0 : cur].start;
        else if (CD->setloc) at = CD->target >= 150 ? CD->target - 150 : 0;
        if (t || CD->setloc) CD->last_lba = at;   // after a seek AutoPause sees no track change; resuming where it paused pauses again (CD Audio Commands, AutoPause)
        stop_read(m); CD->setloc = CD->have_header = 0; CD->lba = at; CD->playing = 1; CD->stat = 0x42;
        CD->sector_next = m->cycles + FIRST + period(m); mechanics(m); sched_update(m); break;
    }
    case 0x04: case 0x05:   // Forward/Backward work only while playing
        if (!CD->playing) { error(m, 0x80); break; }
        RED_ONCE(m, RED_CD_SCAN, "cdrom: Forward/Backward skip sectors at a rate psx-spx does not give: here play goes on at normal speed");
        reply(m, 3, &s, 1, FIRST); break;
    case 0x06: case 0x1b:
        // Read Commands: an unlicensed disc answers INT5 03h 40h, and no INT3, on a console without a modchip; so does an audio disc unless CDDA mode is on
        if (!CD->region && !(CD->mode & 1)) {
            RED_ONCE(m, RED_CD_ID, "cdrom: the disc has no licence string in sector 4, so it reads as an unlicensed disc on a console without a modchip");
            error(m, 0x40); break;
        }
        reply(m, 3, &s, 1, FIRST);
        if (CD->setloc || !CD->reading) {
            if (CD->setloc) { CD->lba = CD->target >= 150 ? CD->target - 150 : UINT32_MAX; CD->setloc = 0; }
            else CD->lba = CD->have_header ? CD->last_lba : CD->lba;
            CD->have_header = CD->data_pending = CD->end_pending = CD->playing = CD->report_n = 0; CD->stat = (s & 0x1f) | 0x42;
            CD->reading = 1; CD->sector_next = m->cycles + FIRST + period(m); mechanics(m); sched_update(m);
        }
        break;
    case 0x07:   // MotorOn; physical spin-up time is still open
        if (s & 2) { error(m, 0x20); break; }   // Control Commands: already spinning is an error, and must not disturb an active read
        mechanics(m); reply(m, 3, &s, 1, FIRST); s = 2; CD->complete_stat = s; reply_later(m, 2, &s, 1, period(m)); break;
    case 0x08: case 0x09: {
        uint32_t delay = c == 8 ? (s & 2 ? (CD->mode & 0x80 ? 0x18a6076 : 0xd38aca) : 0x1d7b) :
            (CD->reading || CD->playing ? (CD->mode & 0x80 ? 0x10bd93 : 0x21181c) : 0x1df2);
        stop_read(m); reply(m, 3, &s, 1, FIRST); s = c == 8 ? 0 : 2; CD->complete_stat = s; reply_later(m, 2, &s, 1, delay); break;
    }
    case 0x0a:
        stop_read(m); CD->mode = CD->size_mode = 0x20; CD->filter_file = CD->filter_channel = 0; CD->data_n = CD->data_pos = CD->have_sector = CD->have_header = 0;
        reply(m, 3, &s, 1, FIRST_INIT); s = mounted ? 2 : 0; CD->complete_stat = s;
        reply_later(m, 2, &s, 1, mounted ? period(m) : SECOND); if (mounted) mechanics(m); break;
    case 0x0b: case 0x0c: CD->muted = c == 0x0b; reply(m, 3, &s, 1, FIRST); break;   // CD audio out at volume zero, while its sectors are still processed
    case 0x0d: CD->filter_file = CD->param[0]; CD->filter_channel = CD->param[1]; reply(m, 3, &s, 1, FIRST); break;
    case 0x0e:   // Setmode: bit 4 keeps the sector size of the last Setmode without it
        CD->mode = CD->param[0];
        if (!(CD->mode & 0x10)) CD->size_mode = CD->mode & 0x20;
        else RED_ONCE(m, RED_CD_IGNORE, "cdrom: Setmode bit 4 also starts the console's reads 0-3 sectors before the Setloc target, at random; here at the target");
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
    case 0x12:   // SetSession: 0 is invalid, 1 seeks to the only session a BIN has, a higher one fails its seek; refused during a read or play
        if (!CD->param[0]) { error(m, 0x10); break; }
        if (CD->reading || CD->playing) { error(m, 0x80); break; }
        reply(m, 3, &s, 1, FIRST); mechanics(m); CD->stat = 0x42; CD->lba = CD->last_lba = 0; CD->have_header = 0;
        if (CD->param[0] == 1) { s = 2; CD->complete_stat = s; reply_later(m, 2, &s, 1, period(m)); }
        else {
            RED_ONCE(m, RED_CD_SESSION, "cdrom: SetSession past the last session: the console's second INT5 and its motor stop are not modelled");
            uint8_t r[2] = {6, 0x40}; CD->complete_stat = 6; reply_later(m, 5, r, 2, period(m));
        }
        break;
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
        // GetID: a licensed disc answers INT2 with "SCEx"; an unlicensed one INT5 with flags 80h (90h with audio tracks); an audio disc is unlicensed, of type 00h
        uint8_t r[8] = {0x08, 0x40}; reply(m, 3, &s, 1, FIRST);
        if (mounted && CD->region) {
            uint8_t ok[8] = {s, 0, 0x20, 0, 'S', 'C', 'E', CD->region}; reply_later(m, 2, ok, 8, SECOND); break;
        }
        if (mounted) {
            r[0] = s | 8; r[1] = 0x80; r[2] = m->disc.track[0].mode == 2 ? 0x20 : 0;
            for (int i = 0; i < m->disc.ntracks; i++) if (m->disc.track[i].audio) r[1] |= 0x10;
            RED_ONCE(m, RED_CD_ID, "cdrom: the disc has no licence string in sector 4, so it reads as an unlicensed disc on a console without a modchip");
        }
        reply_later(m, 5, r, 8, SECOND); break;
    }
    case 0x1c: stop_read(m); CD->scex = CD->region != 0; reply(m, 3, &s, 1, FIRST); break;   // Reset reads the SCEx string again, as a new disc would
    case 0x1d: {   // GetQ: the lead-in's subchannel Q for ADR 1 and a point the TOC has; a BIN has no lead-in, so its own address bytes are 0
        int adr = CD->param[0] & 15, point = CD->param[1], t = unbcd((uint8_t)point), last = m->disc.ntracks - 1; uint8_t q[11] = {1, 0, (uint8_t)point};
        reply(m, 3, &s, 1, FIRST); mechanics(m);
        int found = adr == 1 && ((t >= 1 && t <= m->disc.ntracks) || (point >= 0xa0 && point <= 0xa2));
        if (!found) {
            RED_ONCE(m, RED_CD_GETQ, "cdrom: GetQ for ADR %d point %02x, which the TOC lacks: the console times out after about six seconds, then plays; neither is modelled", adr, point);
            uint8_t r[2] = {(uint8_t)(s | 1), 0x80}; reply_later(m, 5, r, 2, 6 * CPU_HZ); break;
        }
        const ps1_disc_track *tr = &m->disc.track[t >= 1 && t <= m->disc.ntracks ? t - 1 : point == 0xa0 ? 0 : last];
        q[0] |= tr->audio ? 0 : 0x40;
        if (point == 0xa0) { q[7] = 1; q[8] = m->disc.track[0].mode == 2 ? 0x20 : 0; }
        else if (point == 0xa1) q[7] = bcd((uint32_t)m->disc.ntracks);
        else msf((point == 0xa2 ? m->disc.sectors : tr->start) + 150, q + 7);
        reply_later(m, 2, q, 11, period(m)); break;
    }
    case 0x1e:
        stop_read(m); reply(m, 3, &s, 1, FIRST_INIT);
        if (mounted) { s = 2; CD->complete_stat = s; reply_later(m, 2, &s, 1, CPU_HZ); }
        else { uint8_t r[2] = {(uint8_t)(s | 1), 0x80}; reply_later(m, 5, r, 2, SECOND); } break;
    }
}

static void queued(ps1 *m) {
    if (CD->hintsts & 7 || CD->next != NEVER) return;
    if (CD->data_pending && CD->filter_drop) CD->data_pending = 0;   // a later delivery attempt checks file and channel (Data/ADPCM Sector Filtering/Delivery)
    if (CD->later) { CD->later = 0; reply(m, CD->later_int, CD->later_resp, CD->later_n, CD->later_delay); }
    else if (CD->pending) { CD->pending = 0; command(m, CD->pending_cmd); }
    else if (CD->report_n) { reply(m, 1, CD->report, CD->report_n, FIRST); CD->report_n = 0; }
    else if (CD->data_pending) { uint8_t s = read_stat(m) | 0x22; reply(m, 1, &s, 1, FIRST); }   // controller mainloop delay leaves time for ACK-before-BFRD
    else if (CD->end_pending) {
        uint8_t r[2] = {CD->stat, 0x80}, type = CD->end_pending; CD->end_pending = 0;
        if (type == 5) r[0] |= 1;
        reply(m, type, r, type == 5 ? 2 : 1, FIRST);
    }
}
static void ack(ps1 *m, uint8_t v) {
    if (v & 0x40) CD->nparam = 0;
    if (v & 0x20 && CD->audio_xa) CD->audio_n = 0;   // SMADPCLR clears the XA-ADPCM buffer, stopping its playback
    CD->hintsts &= (uint8_t)~(v & 0x1f);
    if (v & 7) CD->nresp = CD->rpos = 0;
    irq_check(m); queued(m);
}
void cd_reset(ps1 *m) {
    memset(CD, 0, sizeof *CD); CD->next = CD->sector_next = NEVER; CD->complete_stat = -1;
    CD->stat = m->disc.ntracks ? 2 : 0; CD->target = 150; CD->zz_six = 6;
    CD->atv[0] = CD->atv[2] = CD->atv_next[0] = CD->atv_next[2] = 0x80;   // normal stereo; psx-spx gives no power-up values
    CD->region = licence(m); CD->scex = CD->region != 0;
}
void cd_event(ps1 *m) {
    CD->next = NEVER;
    if (CD->complete_stat >= 0 && (CD->now_int == 2 || CD->now_int == 5)) { CD->stat = (uint8_t)CD->complete_stat; CD->complete_stat = -1; }
    if (CD->now_int == 1 && !CD->playing) { CD->ready = CD->head; CD->have_sector = 1; CD->data_pending = 0; }
    CD->hintsts = (uint8_t)((CD->hintsts & ~7) | CD->now_int);
    memcpy(CD->resp, CD->now_resp, CD->now_n); CD->nresp = CD->now_n; CD->rpos = 0;
    if (!CD->pending) CD->busy = 0;
    irq_check(m);
}

static void push(ps1 *m, int16_t l, int16_t r, int xa) {
    if (CD->audio_n == CD_AUDIO_FRAMES) { RED_ONCE(m, RED_CD_AUDIO, "cdrom: CD audio arrived faster than the SPU played it: frames dropped"); return; }
    unsigned at = (CD->audio_read + CD->audio_n++) % CD_AUDIO_FRAMES;
    CD->audio[at * 2] = l; CD->audio[at * 2 + 1] = r; CD->audio_xa = xa;
}
// XA-ADPCM's 25-point zigzag interpolation (CDROM Format): each six 37.8 kHz samples become seven at 44.1 kHz, through seven 29-tap tables, here one per row
static const int16_t zigzag[7][29] = {
    {0, 0, 0, 0, 0, -0x2, 0xa, -0x22, 0x41, -0x54, 0x34, 0x9, -0x10a, 0x400, -0xa78, 0x234c, 0x6794, -0x1780, 0xbcd, -0x623, 0x350, -0x16d, 0x6b, 0xa, -0x10, 0x11, -0x8, 0x3, -0x1},
    {0, 0, 0, -0x2, 0, 0x3, -0x13, 0x3c, -0x4b, 0xa2, -0xe3, 0x132, -0x43, -0x267, 0xc9d, 0x74bb, -0x11b4, 0x9b8, -0x5bf, 0x372, -0x1a8, 0xa6, -0x1b, 0x5, 0x6, -0x8, 0x3, -0x1, 0},
    {0, 0, -0x1, 0x3, -0x2, -0x5, 0x1f, -0x4a, 0xb3, -0x192, 0x2b1, -0x39e, 0x4f8, -0x5a6, 0x7939, -0x5a6, 0x4f8, -0x39e, 0x2b1, -0x192, 0xb3, -0x4a, 0x1f, -0x5, -0x2, 0x3, -0x1, 0, 0},
    {0, -0x1, 0x3, -0x8, 0x6, 0x5, -0x1b, 0xa6, -0x1a8, 0x372, -0x5bf, 0x9b8, -0x11b4, 0x74bb, 0xc9d, -0x267, -0x43, 0x132, -0xe3, 0xa2, -0x4b, 0x3c, -0x13, 0x3, 0, -0x2, 0, 0, 0},
    {-0x1, 0x3, -0x8, 0x11, -0x10, 0xa, 0x6b, -0x16d, 0x350, -0x623, 0xbcd, -0x1780, 0x6794, 0x234c, -0xa78, 0x400, -0x10a, 0x9, 0x34, -0x54, 0x41, -0x22, 0xa, -0x1, 0, 0x1, 0, 0, 0},
    {0x2, -0x8, 0x10, -0x23, 0x2b, 0x1a, -0xeb, 0x27b, -0x548, 0xafa, -0x16fa, 0x53e0, 0x3c07, -0x1249, 0x80e, -0x347, 0x15b, -0x44, -0x17, 0x46, -0x23, 0x11, -0x5, 0, 0, 0, 0, 0, 0},
    {-0x5, 0x11, -0x23, 0x46, -0x17, -0x44, 0x15b, -0x347, 0x80e, -0x1249, 0x3c07, 0x53e0, -0x16fa, 0xafa, -0x548, 0x27b, -0xeb, 0x1a, 0x2b, -0x23, 0x10, -0x8, 0x2, 0, 0, 0, 0, 0, 0},
};
static void xa_out(ps1 *m, int16_t l, int16_t r) {
    CD->zz[0][CD->zz_p & 31] = l; CD->zz[1][CD->zz_p & 31] = r; CD->zz_p++;
    if (--CD->zz_six) return;
    CD->zz_six = 6;
    for (int k = 0; k < 7; k++) {
        int32_t o[2] = {0, 0};
        for (int c = 0; c < 2; c++) for (int i = 1; i <= 29; i++) o[c] += CD->zz[c][(CD->zz_p - (unsigned)i) & 31] * zigzag[k][i - 1] >> 15;
        push(m, sat(o[0]), sat(o[1]), 1);
    }
}
// An XA-ADPCM sector (CDROM XA Audio ADPCM Compression): eighteen 128-byte portions of 4-bit (8 blocks) or 8-bit (4 blocks) samples, mono, or stereo with the
// even blocks left; each block has its own shift and one of four predictors, whose history runs on across sectors. 18.9 kHz samples go in twice
static void xa_sector(ps1 *m, const uint8_t *s) {
    static const int pos[4] = {0, 60, 115, 98}, neg[4] = {0, 0, -52, -55};
    uint8_t ci = s[19]; int stereo = (ci & 3) == 1, eight = (ci >> 4 & 3) == 1, blocks = eight ? 4 : 8;
    if (ci & 0xea) RED_ONCE(m, RED_CD_XA, "cdrom: XA-ADPCM coding info %02x: reserved bits, or emphasis (whose formula psx-spx does not give), are ignored", ci);
    int16_t out[2][18 * 8 * 28]; int n[2] = {0, 0};
    for (int i = 0; i < 18; i++) {
        const uint8_t *p = s + 24 + i * 128;
        for (int b = 0; b < blocks; b++) {
            int ch = stereo ? b & 1 : 0, shift = p[4 + b] & 15, f = p[4 + b] >> 4 & 3;
            if (shift > 12) shift = 9;
            for (int j = 0; j < 28; j++) {
                int t = eight ? (int8_t)p[16 + b + j * 4] * 256 : ((p[16 + b / 2 + j * 4] >> (b & 1) * 4 & 15) ^ 8) * 4096 - 8 * 4096;
                int32_t v = (t >> shift) + ((CD->xa_old[ch] * pos[f] + CD->xa_older[ch] * neg[f] + 32) >> 6);
                CD->xa_older[ch] = CD->xa_old[ch]; CD->xa_old[ch] = out[ch][n[ch]++] = sat(v);
            }
        }
    }
    for (int k = 0; k < n[0]; k++)
        for (int twice = 0; twice <= (ci >> 2 & 1); twice++) xa_out(m, out[0][k], out[stereo][k]);
}
// Play (CD Audio Commands): audio sectors to the SPU, data sectors as silence; at double speed every other frame, as the drive plays twice as fast. AutoPause
// (Setmode.1) pauses at a track change with INT4; Report (Setmode.2) sends INT1 every ten sectors, the absolute time at frames 0, 20, 40, 60, the time in the track
// (seconds +80h) at 10, 30, 50, 70, with the peak level
static void play(ps1 *m, const uint8_t *s, int t, int audio) {
    int prev = disc_track(m, CD->last_lba);
    if (CD->mode & 2 && prev >= 0 && t != prev) { CD->playing = 0; CD->sector_next = NEVER; CD->stat = 2; CD->end_pending = 4; queued(m); return; }
    int peak = 0;
    for (int i = 0; i < 588; i++) {
        int16_t l = audio ? (int16_t)(s[i * 4] | s[i * 4 + 1] << 8) : 0, r = audio ? (int16_t)(s[i * 4 + 2] | s[i * 4 + 3] << 8) : 0;
        if (l > peak || -l > peak) peak = l < 0 ? -l : l;
        if (!(CD->mode & 0x80) || !(i & 1)) push(m, l, r, 0);
    }
    CD->last_lba = CD->lba++; CD->stat = 0x82;
    uint32_t at = CD->last_lba + 150, f = at % 75; const ps1_disc_track *tr = &m->disc.track[t];
    if (!(CD->mode & 4) || f % 10) return;
    RED_ONCE(m, RED_CD_PEAK, "cdrom: Report gives the left channel's peak; which channel the console's L/R flag settles on is not documented");
    uint8_t *r = CD->report; r[0] = CD->stat; r[1] = bcd((uint32_t)t + 1); r[2] = CD->last_lba < tr->start ? 0 : 1;
    if (f / 10 % 2 == 0) msf(at, r + 3);
    else { msf(r[2] ? CD->last_lba - tr->start : tr->start - CD->last_lba, r + 3); r[4] |= 0x80; }
    peak = peak > 0x7fff ? 0x7fff : peak; r[6] = (uint8_t)peak; r[7] = (uint8_t)(peak >> 8); CD->report_n = 8;
    if (idle(m)) { CD->report_n = 0; CD->now_int = 1; CD->now_n = 8; memcpy(CD->now_resp, r, 8); cd_event(m); }
}

void cd_sector_event(ps1 *m) {
    CD->sector_next += period(m);   // Response Timings: exact rate, independent of controller responses and acknowledgements
    if (CD->lba >= m->disc.sectors) {   // the lead-out: a read stops there with the motor on; play stops the motor, or with AutoPause pauses (Play, AutoPause)
        CD->stat = CD->playing && !(CD->mode & 2) ? 0 : 2; CD->reading = CD->playing = 0; CD->sector_next = NEVER; CD->end_pending = 4; queued(m); return;
    }
    uint8_t s[DISC_SECTOR];
    if (disc_read(m, CD->lba, s)) {
        RED("cdrom: external sector read failed"); CD->reading = CD->playing = 0; CD->sector_next = NEVER; CD->stat = 2; CD->end_pending = 5; queued(m); return;
    }
    int t = disc_track(m, CD->lba), audio = t >= 0 && m->disc.track[t].audio;
    if (CD->playing) { play(m, s, t, audio); return; }
    if (audio && !(CD->mode & 1)) { CD->reading = 0; CD->sector_next = NEVER; CD->stat = 2; CD->end_pending = 5; queued(m); return; }
    if (audio) CD->have_header = 0; else { memcpy(CD->header, s + 12, 8); CD->have_header = 1; }
    CD->last_lba = CD->lba++; CD->stat = 0x22;
    // Data/ADPCM Sector Filtering/Delivery: a Mode2 sector whose submode is audio and real-time goes to the XA-ADPCM decoder when Setmode.6 is on and, with Setmode.3,
    // its file and channel match Setfilter; with Setmode.3 it never reaches the CPU. Other sectors reach the CPU, at the first attempt whatever their file and channel
    int xa = !audio && s[15] == 2 && (s[18] & 0x44) == 0x44, match = s[16] == CD->filter_file && s[17] == CD->filter_channel;
    if (xa && CD->mode & 0x40 && (!(CD->mode & 8) || match)) { xa_sector(m, s); return; }
    if (xa && CD->mode & 8) return;
    uint8_t next = (CD->head + 1) & 7;
    if (CD->have_sector && next == CD->ready && CD->data_pos == CD->data_n)
        RED_ONCE(m, RED_CD_OVERRUN, "cdrom: sector overrun uses whole decoder slots; partial-sector overwrite timing is not modelled yet");
    memcpy(CD->sector[next], s, DISC_SECTOR); CD->head = next;
    CD->data_pending = 1; CD->filter_drop = CD->mode & 8 && !match;
    if (idle(m)) { CD->now_int = 1; CD->now_n = 1; CD->now_resp[0] = read_stat(m); cd_event(m); }
}
void cd_audio(ps1 *m, int16_t out[2]) {
    int32_t l = 0, r = 0;
    if (CD->audio_n) { l = CD->audio[CD->audio_read * 2]; r = CD->audio[CD->audio_read * 2 + 1]; CD->audio_read = (CD->audio_read + 1) % CD_AUDIO_FRAMES; CD->audio_n--; }
    if (CD->muted || (CD->audio_xa && CD->adp_mute)) l = r = 0;
    // ATV0 L->L, ATV1 L->R, ATV2 R->R, ATV3 R->L, 80h for unity; the console saturates up to double volume (CDROM Controller I/O Ports, ATV0-ATV3)
    out[0] = sat((l * CD->atv[0] + r * CD->atv[3]) >> 7); out[1] = sat((r * CD->atv[2] + l * CD->atv[1]) >> 7);
}
static uint8_t data_read(ps1 *m) {
    if (CD->data_pos < CD->data_n) return CD->data[CD->data_pos++];
    return CD->data_n ? CD->data[CD->data_n - (CD->data_n == 2048 ? 8 : 4)] : 0;   // RDDATA padding after the FIFO empties
}
uint8_t cd_read(ps1 *m, uint32_t off) {
    switch (off) {
    case 0: return (uint8_t)(CD->index | (CD->audio_n && CD->audio_xa) << 2 | (CD->nparam == 0) << 3 | (CD->nparam < 16) << 4 | (CD->rpos < CD->nresp) << 5 |
                             (CD->data_pos < CD->data_n) << 6 | CD->busy << 7);
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
        if (v & 0x20) RED_ONCE(m, RED_CD_SOUNDMAP, "cdrom: sound map XA-ADPCM playback (HCHPCTL.5, WRDATA) is not modelled");
        if (!(v & 0x80)) CD->data_n = CD->data_pos = 0;
        else if (CD->have_sector && CD->data_pos == CD->data_n) {
            const uint8_t *s = CD->sector[CD->ready]; int off = CD->size_mode ? 12 : s[15] == 1 ? 16 : 24;
            CD->data_n = CD->size_mode ? 2340 : 2048; CD->data_pos = 0; memcpy(CD->data, s + off, CD->data_n);
        }
        break;
    case 3 << 2 | 1: ack(m, v); break;
    case 2 << 2 | 2: CD->atv_next[0] = v; break;
    case 3 << 2 | 2: CD->atv_next[1] = v; break;
    case 1 << 2 | 3: CD->atv_next[2] = v; break;
    case 2 << 2 | 3: CD->atv_next[3] = v; break;
    case 3 << 2 | 3: CD->adp_mute = v & 1; if (v & 0x20) memcpy(CD->atv, CD->atv_next, 4); break;   // ADPCTL: ADPMUTE, CHNGATV
    default: RED_ONCE(m, RED_CD_SOUNDMAP, "cdrom: sound map XA-ADPCM playback (HCHPCTL.5, WRDATA) is not modelled"); break;   // WRDATA, CI
    }
}
uint32_t cd_dma_read(ps1 *m) {
    if (!CD->data_n) RED_ONCE(m, RED_CD, "cdrom: forced DMA read with no sector in the host FIFO");
    uint32_t w = 0; for (int i = 0; i < 4; i++) w |= (uint32_t)data_read(m) << (i * 8); return w;
}
