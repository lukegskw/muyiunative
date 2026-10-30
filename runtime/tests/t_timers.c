// The root counters (timers.c; psx-spx, Timers): counting the system clock, by 8, and hblanks; a target reached with ZRET (counted up to and including the target, then 0),
// FFFFh and the wrap, the reached-flags that reset when read, one-shot and toggle IRQs, a count written above the target, which runs to FFFFh first, and the four
// synchronization modes to hblanks and vblanks. The CPU's time passes event by event, as cpu_run and sched_run make it pass
#include "ps1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

enum { COUNT = 0, MODE = 4, TARGET = 8, ZRET = 8, CMP = 16, OVFL = 32, REPT = 64, LEVL = 128, EXTC = 256, PSCL = 512 };
static ps1 *m;
static uint32_t rd(int n, int reg) { return mem_read32(m, 0x1f801100 + 16 * (uint32_t)n + (uint32_t)reg); }
static void wr(int n, int reg, uint32_t v) { mem_write32(m, 0x1f801100 + 16 * (uint32_t)n + (uint32_t)reg, v); }
static void pass(uint64_t cycles) {   // as the CPU would: to each due event, then to the end
    uint64_t end = m->cycles + cycles;
    while (m->cycles < end) { m->cycles = m->next_event < end ? m->next_event : end; sched_run(m); }
}
static int irq(int n) { int on = m->i_stat >> (IRQ_TMR0 + n) & 1; m->i_stat = 0; irq_update(m); return on; }
static void to_line(int l) { do pass(m->gpu.next_line - m->cycles); while (m->gpu.line != l); }   // to the first cycle of scanline l

int main(void) {
    m = ps1_new();
    if (!m) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }

    wr(2, MODE, 0); pass(1000);
    CHECK(rd(2, COUNT) == 1000, "timer 2 counts the system clock");
    wr(2, MODE, PSCL); pass(800);
    CHECK(rd(2, COUNT) == 100, "timer 2 counts the system clock by 8");

    wr(2, TARGET, 99); wr(2, MODE, ZRET | CMP | REPT); pass(98);
    CHECK(rd(2, COUNT) == 98 && !irq(2), "below the target: no IRQ");
    pass(1);
    CHECK(rd(2, COUNT) == 99 && irq(2), "the target is a count of its own, and reaching it raises IRQ 6");
    pass(1);
    CHECK(rd(2, COUNT) == 0, "after the target, ZRET restarts at 0");
    uint32_t mode = rd(2, MODE);
    CHECK(mode >> 11 & 1 && !(rd(2, MODE) >> 11 & 1), "the reached-target flag, reset by reading MODE");
    pass(250);
    CHECK(rd(2, COUNT) == 50 && irq(2), "repeat mode: an IRQ every round of target + 1 counts");

    wr(1, MODE, OVFL | REPT); wr(1, COUNT, 0xfff0); pass(15);
    CHECK(rd(1, COUNT) == 0xffff && irq(1) && rd(1, MODE) >> 12 & 1, "reaching FFFFh: IRQ 5 and its flag");
    pass(1);
    CHECK(rd(1, COUNT) == 0, "then 0");

    wr(0, TARGET, 9); wr(0, MODE, ZRET | CMP); pass(10);
    CHECK(irq(0), "one-shot: the first time");
    pass(10);
    CHECK(!irq(0), "one-shot: not again until MODE is written");
    wr(0, MODE, ZRET | CMP | REPT | LEVL); pass(10);
    CHECK(irq(0) && !(rd(0, MODE) >> 10 & 1), "toggle: INTF falls, an IRQ");
    pass(10);
    CHECK(!irq(0) && rd(0, MODE) >> 10 & 1, "toggle: INTF rises, no IRQ");
    pass(10);
    CHECK(irq(0), "toggle: falls again, an IRQ");

    wr(0, TARGET, 10); wr(0, MODE, ZRET | CMP | REPT); wr(0, COUNT, 20); pass(100);
    CHECK(rd(0, COUNT) == 120 && !irq(0), "a count above the target runs on past it");
    pass(0xffff - 120 + 1 + 10);
    CHECK(rd(0, COUNT) == 10 && irq(0), "to FFFFh, then 0, then the target");

    uint64_t line0 = m->gpu.next_line;
    wr(1, MODE, EXTC); m->cycles = line0 - 1; sched_run(m);
    uint32_t start = rd(1, COUNT);
    pass(1); pass(9 * 2153);   // a scanline is 2152.85 cycles
    CHECK(rd(1, COUNT) - start == 10, "timer 1 counts hblanks, one a scanline");

    // Synchronization. After GP1(00h) the display range is X1 = 200h to X2 = C00h video cycles, CPU cycles 322 and 1937 of a 2152.85-cycle scanline, the rest of it
    // hblank; the lines outside Y1 = 16 to Y2 = 256 are vblank (psx-spx, GP1(00h), GP1(06h), GP1(07h))
    enum { GATF = 1, X1 = 322, X2 = 1937 };
    to_line(100); wr(0, MODE, GATF | 0 << 1); pass(X1 + 100);
    CHECK(rd(0, COUNT) == 100, "sync 0: timer 0 pauses in the hblank, until X1");
    pass(X2 - X1 - 100 + 50);
    CHECK(rd(0, COUNT) == X2 - X1, "sync 0: and again from X2");
    to_line(101); pass(X1 + 10);
    CHECK(rd(0, COUNT) == X2 - X1 + 10, "sync 0: through the next scanline's hblank");

    to_line(102); wr(0, MODE, GATF | 1 << 1); pass(X2 + 100);
    CHECK(rd(0, COUNT) == 100, "sync 1: timer 0 counts on, reset to 0 where the hblank starts");

    to_line(103); uint64_t s = m->gpu.line_start; wr(0, MODE, GATF | 2 << 1); pass(300);
    CHECK(rd(0, COUNT) == 300, "sync 2: timer 0 counts inside the hblank");
    pass(X1 - 300 + 50);
    CHECK(rd(0, COUNT) == 0, "sync 2: and reads 0 outside it (ps1-tests timers log)");
    pass(X2 - X1 - 50 + 100);
    CHECK(rd(0, COUNT) == 100, "sync 2: from 0 where the next hblank starts");
    to_line(104); pass(10);
    CHECK(rd(0, COUNT) == m->cycles - s - X2, "sync 2: on into the next scanline");

    to_line(105); pass(400); s = m->gpu.line_start; wr(0, MODE, GATF | 3 << 1); pass(X2 - 400 - 1);
    CHECK(rd(0, COUNT) == 0, "sync 3: timer 0 waits for an hblank to start");
    to_line(107); pass(X1 + 10);
    CHECK(rd(0, COUNT) == m->cycles - s - X2, "sync 3: then runs freely");

    mem_write32(m, 0x1f801814, 0x06000000 | 0x200 | 0x200 << 12); wr(0, MODE, GATF); wr(1, MODE, EXTC); to_line(110); pass(1000);
    CHECK(rd(0, COUNT) == 0 && rd(1, COUNT) == 0, "a display range 0 pixels wide: an endless hblank, and no hblank for timer 1 to count (GPU Timings)");
    mem_write32(m, 0x1f801814, 0x06000000 | 0x200 | 0xc00 << 12); wr(0, MODE, 0);

    to_line(200); wr(1, MODE, EXTC | GATF | 1 << 1); to_line(256 + 5);
    CHECK(rd(1, COUNT) == 5, "sync 1: timer 1 counting hblanks from where the vblank starts");
    to_line(250); wr(1, MODE, GATF | 0 << 1); to_line(256);
    uint32_t c = rd(1, COUNT); to_line(15);
    CHECK(rd(1, COUNT) == c, "sync 0: timer 1 pauses in the vblank");
    to_line(16); pass(100);
    CHECK(rd(1, COUNT) == c + 100, "sync 0: and goes on after it");
    to_line(250); wr(1, MODE, GATF | 2 << 1); pass(100);
    CHECK(rd(1, COUNT) == 0, "sync 2: timer 1 does not count outside the vblank");
    to_line(256); pass(100);
    CHECK(rd(1, COUNT) == 100, "sync 2: counts inside it");
    to_line(16);
    CHECK(rd(1, COUNT) == 0, "sync 2: and reads 0 after it");
    to_line(100); wr(1, MODE, GATF | 3 << 1); to_line(255);
    CHECK(rd(1, COUNT) == 0, "sync 3: timer 1 waits for the vblank");
    to_line(256); pass(100);
    CHECK(rd(1, COUNT) == 100, "sync 3: then runs");

    wr(2, MODE, GATF | 0 << 1); pass(100);
    CHECK(rd(2, COUNT) == 0, "sync 0 and 3 stop timer 2");
    wr(2, MODE, GATF | 2 << 1); pass(100);
    CHECK(rd(2, COUNT) == 100, "sync 1 and 2 let it run");

    printf("timers: %d checks, %d failed\n", checks, fails);
    ps1_free(m);
    return fails != 0;
}
