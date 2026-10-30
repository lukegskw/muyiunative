// The root counters (psx-spx, Timers): three 16-bit counters. Timer 0 counts the system clock or the dotclock, timer 1 the system clock or hblanks, timer 2 the system
// clock, optionally divided by 8. With MODE.ZRET a counter counts up to and including its target and restarts at 0, otherwise it wraps after FFFFh (a count written above
// the target also runs to FFFFh first); reaching the target or FFFFh sets a flag and, when enabled, raises IRQ 4..6 (pulse or toggle, once or repeatedly). Clock counts
// are brought up to date when read and at the cycles their IRQs fall due; hblanks are counted per scanline (gpu_line).
// MODE.0 synchronizes timer 0 to hblanks and timer 1 to vblanks (gpu_sw.c says where they lie): 0 pauses the counter inside the blank, 1 resets it to 0 where the blank
// starts, 2 counts only inside the blank, from 0, and 3 waits for the first blank to start, then runs freely. In mode 2 the counter reads 0 outside the blank: the
// ps1-tests timers log shows it count from 0 inside the hblank and read 0 again after it, so it is reset where the blank ends as well. Timer 2's modes 0 and 3 stop it.
// Not modelled: the cycles a counter holds 0 after a reset (Reset and Wrap), and timer 0 counting a whole number of dots a scanline (GPU Timings, Horizontal Timings)
#include "ps1.h"
#include <string.h>

enum { T_GATF = 1, T_ZRET = 8, T_CMP = 16, T_OVFL = 32, T_REPT = 64, T_LEVL = 128, T_EXTC = 256, T_PSCL = 512, T_INTF = 1024, T_EQUF = 2048, T_OVFF = 4096 };

static uint64_t period_fp(const ps1 *m, int n) {   // CPU cycles per count, 16.16; 0 when not counting the system or dot clock (stopped, paused, or counting hblanks)
    uint32_t mode = m->tmr[n].mode;
    if (n == 2) return mode & T_GATF && (mode >> 1 & 3) % 3 == 0 ? 0 : (uint64_t)(mode & T_PSCL ? 8 : 1) << 16;   // sync modes 0 and 3 stop timer 2
    if (mode & T_GATF && !m->tmr[n].gate) return 0;
    if (!(mode & T_EXTC)) return 1 << 16;
    return n == 0 ? gpu_dot_fp(m) : 0;
}

static uint32_t lap(const ps1_timer *t) { return t->mode & T_ZRET ? t->target + 1u : 0x10000; }

static uint64_t ticks_to(const ps1_timer *t, uint32_t v) {   // counts until the counter next becomes v; NEVER when it cannot
    uint32_t c = t->count, l = lap(t);
    if (c >= l) return v > c ? v - c : v < l ? 0x10000 - c + v : NEVER;   // above its target: up to FFFFh, then round 0..target
    if (v >= l) return NEVER;
    return v > c ? v - c : l - c + v;
}

static uint16_t after(const ps1_timer *t, uint64_t k) {   // the counter after k counts
    uint32_t c = t->count, l = lap(t);
    if (c >= l) { if (k < 0x10000 - c) return (uint16_t)(c + k); k -= 0x10000 - c; c = 0; }
    return (uint16_t)((c + k) % l);
}

static int can_fire(const ps1_timer *t) { return t->mode & (T_CMP | T_OVFL) && (t->mode & T_REPT || !t->fired); }   // one-shot: once until MODE is written

static void fire(ps1 *m, int n) {
    ps1_timer *t = &m->tmr[n];
    if (!(t->mode & T_REPT) && t->fired) return;
    t->fired = 1;
    if (t->mode & T_LEVL) { t->mode ^= T_INTF; if (t->mode & T_INTF) return; }   // toggle: an IRQ each time INTF falls
    irq_raise(m, IRQ_TMR0 + n);
}

static void advance(ps1 *m, int n, uint64_t k) {   // k counts: the target and FFFFh on the way set their flags and raise their IRQs
    ps1_timer *t = &m->tmr[n];
    while (k) {
        uint64_t dt = ticks_to(t, t->target), df = ticks_to(t, 0xffff), d = dt < df ? dt : df;
        if (d > k) break;
        k -= d; t->count = (uint16_t)(dt <= df ? t->target : 0xffff);
        if (t->count == t->target) { t->mode |= T_EQUF; if (t->mode & T_CMP) fire(m, n); }
        if (t->count == 0xffff) { t->mode |= T_OVFF; if (t->mode & T_OVFL) fire(m, n); }
        if (!can_fire(t) && t->count < lap(t)) k %= lap(t);   // whole rounds would only set the flags again
    }
    t->count = after(t, k);
}

static void sync_to(ps1 *m, int n, uint64_t at) {   // counts until cycle `at` (an edge already passed may be a little behind the last read)
    ps1_timer *t = &m->tmr[n]; uint64_t p = period_fp(m, n);
    if (at < t->last) at = t->last;
    if (p) { t->acc += (at - t->last) << 16; uint64_t k = t->acc / p; t->acc -= k * p; advance(m, n, k); }
    t->last = at;
}

static void sync(ps1 *m, int n) { sync_to(m, n, m->cycles); }

static void plan(ps1 *m, int n) {   // after sync: the cycle of the next count that may raise an IRQ
    ps1_timer *t = &m->tmr[n]; uint64_t p = period_fp(m, n);
    t->next = NEVER;
    if (!p || !can_fire(t)) return;
    uint64_t dt = t->mode & T_CMP ? ticks_to(t, t->target) : NEVER, df = t->mode & T_OVFL ? ticks_to(t, 0xffff) : NEVER, d = dt < df ? dt : df;
    if (d == NEVER) return;
    uint64_t need = d * p;
    t->next = t->last + (need > t->acc ? (need - t->acc + 0xffff) >> 16 : 0);
}

void tmr_reset(ps1 *m) {
    memset(m->tmr, 0, sizeof m->tmr);
    for (int n = 0; n < 3; n++) { m->tmr[n].mode = T_INTF; m->tmr[n].next = NEVER; }
}

void tmr_event(ps1 *m, int n) { sync(m, n); plan(m, n); }

void tmr_hblank(ps1 *m) { ps1_timer *t = &m->tmr[1]; if (t->mode & T_EXTC && (!(t->mode & T_GATF) || t->gate)) advance(m, 1, 1); }

void tmr_blank(ps1 *m, int n, int start, uint64_t at) {
    ps1_timer *t = &m->tmr[n];
    if (!(t->mode & T_GATF)) return;
    sync_to(m, n, at);
    switch (t->mode >> 1 & 3) {
    case 0: t->gate = !start; break;
    case 1: if (start) t->count = 0; break;
    case 2: t->gate = start; t->count = 0; break;
    case 3: if (start) t->gate = 1; break;
    }
    plan(m, n); sched_update(m);
}

uint32_t tmr_read(ps1 *m, uint32_t off) {
    int n = off >> 4;
    if (n > 2) return 0;
    ps1_timer *t = &m->tmr[n]; sync(m, n); plan(m, n); sched_update(m);
    switch (off & 0xf) {
    case 0: return t->count;
    case 4: { uint32_t v = t->mode; t->mode &= ~(uint32_t)(T_EQUF | T_OVFF); return v; }   // the reached-flags reset when read
    case 8: return t->target;
    }
    return 0;
}

void tmr_write(ps1 *m, uint32_t off, uint32_t v) {
    int n = off >> 4;
    if (n > 2) return;
    ps1_timer *t = &m->tmr[n]; sync(m, n);
    switch (off & 0xf) {
    case 0: t->count = (uint16_t)v; break;
    case 4: {   // writing the mode resets the counter, the one-shot and INTF
        t->mode = (v & 0x3ff) | T_INTF; t->count = 0; t->fired = 0; t->acc = 0;
        uint32_t sm = v >> 1 & 3; int blank = n == 0 ? gpu_hblank_now(m) : n == 1 && gpu_vblank(m);
        t->gate = sm == 1 || (sm == 0 && !blank) || (sm == 2 && blank);   // mode 3 waits for a blank to start
        break;
    }
    case 8: t->target = (uint16_t)v; break;
    }
    plan(m, n); sched_update(m);
}
