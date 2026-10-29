// The root counters (timers.c; psx-spx, Timers): counting the system clock, by 8, and hblanks; a target reached with ZRET (counted up to and including the target, then 0),
// FFFFh and the wrap, the reached-flags that reset when read, one-shot and toggle IRQs, and a count written above the target, which runs to FFFFh first. The CPU's time
// passes event by event, as cpu_run and sched_run make it pass
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

    printf("timers: %d checks, %d failed\n", checks, fails);
    ps1_free(m);
    return fails != 0;
}
