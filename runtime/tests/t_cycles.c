// The CPU's cycles (interp.c, mem.c; psx-spx, CPU Specifications and Memory Control): a cached instruction takes 1 cycle and an uncached one the read of its word; a
// load waits for its data by what it reads (the console's numbers of ps1-tests cpu/access-time); MULT and DIV run beside the CPU and MFLO waits for them. The programs
// are built here from MIPS encodings and stepped with cpu_step, so no scheduler event runs between their instructions
#include "ps1.h"
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static ps1 *m;
enum { NOP = 0, LB = 0x20, LH = 0x21, LW = 0x23, LBU = 0x24, LHU = 0x25 };
static uint32_t load(unsigned op) { return op << 26 | 1u << 21 | 2u << 16; }   // op r2, 0(r1)
static uint32_t muldiv(unsigned funct) { return 1u << 21 | 3u << 16 | funct; }   // funct r1, r3
static const uint32_t MFLO = 2u << 11 | 0x12;

static uint64_t run(uint32_t at, const uint32_t *code, int n) {   // the cycles n instructions at `at` take; code for BFC00000h goes into the BIOS ROM
    uint32_t p = at & 0x1fffffff; uint8_t *to = p >= 0x1fc00000 ? m->bios + (p - 0x1fc00000) : m->ram + (p & (RAM_SIZE - 1));
    memcpy(to, code, 4 * (size_t)n);
    m->cpu.pc = at; m->cpu.next_pc = at + 4; m->cpu.cop0[C0_SR] = 1u << 30;
    uint64_t start = m->cycles;
    for (int i = 0; i < n; i++) cpu_step(m);
    return m->cycles - start;
}

int main(void) {
    m = ps1_new();
    if (!m) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }
    const uint32_t nops[8] = {0};
    CHECK(run(0x80010000, nops, 8) == 8, "cached code in RAM: a cycle an instruction");
    CHECK(run(0xa0010000, nops, 8) == 40, "uncached code in RAM: each instruction a 5-cycle read");
    CHECK(run(0x9fc00000, nops, 4) == 4, "the BIOS through KSEG0 is cached");
    CHECK(run(0xbfc00000, nops, 4) == 100, "the BIOS through KSEG1: a 32-bit read on its 8-bit bus, 25 cycles an instruction");

    static const struct { uint32_t addr; unsigned op, cycles; const char *what; } loads[] = {
        {0x80000000, LW, 5, "a load from RAM takes 5 cycles (5.14 on the console)"},
        {0x80000000, LBU, 5, "whatever its width (5.21)"},
        {0x1f800000, LW, 1, "from the scratchpad, 1 (0.94)"},
        {0x1f801070, LW, 3, "from an internal port, 3 (I_STAT: 3.1)"},
        {0x1f801814, LW, 3, "GPUSTAT too (2.93 to 3.8)"},
        {0xbfc00000, LB, 8, "from the BIOS ROM, a byte takes 8 (7.6)"},
        {0xbfc00000, LH, 13, "a halfword 13 (12.94)"},
        {0xbfc00000, LW, 25, "a word 25 (24.94)"},
        {0x1f801800, LBU, 8, "the CD-ROM's 8-bit port, 8 (8.0)"},
        {0x1f801daa, LHU, 18, "the SPU, 18 (17.99)"},
        {0x1f000000, LBU, 7, "the expansion port, 7 (6.94)"},
        {0x1f802000, LBU, 11, "the second expansion region, 11 (10.99)"},
    };
    for (size_t i = 0; i < sizeof loads / sizeof *loads; i++) {
        uint32_t code[2] = {load(loads[i].op), NOP};
        m->cpu.r[1] = loads[i].addr;
        CHECK(run(0x80010000, code, 2) == loads[i].cycles + 1, loads[i].what);
    }

    uint32_t mul[2] = {muldiv(0x19), MFLO}, spaced[8] = {muldiv(0x19), 0, 0, 0, 0, 0, 0, MFLO};
    m->cpu.r[1] = 0x123; m->cpu.r[3] = 0x12345678;
    CHECK(run(0x80010000, mul, 2) == 8 && m->cpu.r[2] == (uint32_t)(0x123ull * 0x12345678), "MULTU with a small rs runs 6 cycles, and MFLO waits for it");
    CHECK(run(0x80010000, spaced, 8) == 8, "six instructions between MULTU and MFLO hide the wait (psx-spx's example)");
    m->cpu.r[1] = 0x12345;
    CHECK(run(0x80010000, mul, 2) == 11, "an rs below 100000h: 9 cycles");
    m->cpu.r[1] = 0x12345678;
    CHECK(run(0x80010000, mul, 2) == 15, "a larger rs: 13 cycles");
    mul[0] = muldiv(0x18); m->cpu.r[1] = 0xfffff800;
    CHECK(run(0x80010000, mul, 2) == 8, "MULT counts a negative rs by its size: FFFFF800h is fast");
    m->cpu.r[1] = 0xfffff7ff;
    CHECK(run(0x80010000, mul, 2) == 11, "and FFFFF7FFh is not");
    mul[0] = muldiv(0x1b);
    CHECK(run(0x80010000, mul, 2) == 38, "DIVU: 36 cycles");

    printf("cycles: %d checks, %d failed\n", checks, fails);
    ps1_free(m);
    return fails != 0;
}
