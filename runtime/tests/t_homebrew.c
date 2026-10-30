// Three small programs built here from MIPS I encodings and run by the interpreter, through the memory-mapped ports rather than device calls; they complement the
// ps1-tests programs (t_ps1tests.c), plan.md's homebrew test programs. CPU: load and branch delays, arithmetic; GTE: projection, flags and COP2 branches; GPU: an
// upload, a textured sprite and a readback. Expected values come from psx-spx (CPU Specifications; GTE Coordinate Calculation Commands, Division Inaccuracy; GPU
// Render Rectangle Commands, Memory Transfer Commands), never from an emulator. No SDK, game or BIOS is needed
#include "ps1.h"
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static uint32_t code[256]; static unsigned ncode;
static uint32_t ins(unsigned op, unsigned rs, unsigned rt, uint16_t imm) { return op << 26 | rs << 21 | rt << 16 | imm; }
static void emit(uint32_t word) { code[ncode++] = word; }
static void li(unsigned reg, uint32_t v) { emit(ins(15, 0, reg, (uint16_t)(v >> 16))); emit(ins(13, reg, reg, (uint16_t)v)); }   // lui; ori
static void store(unsigned reg, uint16_t at) { li(14, 0xa0000000u | at); emit(ins(43, 14, reg, 0)); }
static void finish(void) { li(15, 0x1f802082); emit(ins(41, 15, 0, 0)); }   // sh zero to the exit port: success
static void gp0(uint32_t word) { li(9, word); emit(ins(43, 8, 9, 0)); }   // t0 holds GP0's address
static int run(ps1 *m) {
    memcpy(m->ram + 0x10000, code, ncode * 4); m->cpu.pc = 0x80010000; m->cpu.next_pc = m->cpu.pc + 4; m->cpu.cop0[C0_SR] = 1u << 30;
    for (int i = 0; i < 4096 && !m->exit_requested; i++) { cpu_step(m); if (m->cycles >= m->next_event) sched_run(m); }
    return m->exit_requested && m->exit_code == 0;
}
static uint32_t word(ps1 *m, unsigned at) { uint32_t v; memcpy(&v, m->ram + at, 4); return v; }

int main(void) {
    ps1 *m = ps1_new();
    if (!m) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }

    ncode = 0;
    emit(ins(9, 0, 8, 5)); emit(ins(9, 0, 9, 7)); emit(8u << 21 | 9u << 16 | 10u << 11 | 0x21);   // addu t2,t0,t1 = 12
    li(14, 0xa0002000); emit(ins(43, 14, 10, 0)); emit(ins(35, 14, 11, 0));
    emit(11u << 21 | 16u << 11 | 0x21);   // addu s0,t3,zero still sees the old t3
    emit(ins(9, 11, 17, 1)); emit(ins(4, 11, 10, 2)); emit(ins(9, 17, 18, 1)); emit(ins(9, 0, 19, 99));
    emit(8u << 21 | 9u << 16 | 0x18); emit(20u << 11 | 0x12);   // mult 5*7; mflo s4
    store(16, 0x2010); store(17, 0x2014); store(18, 0x2018); store(19, 0x201c); store(20, 0x2020); finish();
    CHECK(run(m), "CPU program reaches its success exit port");
    CHECK(word(m, 0x2010) == 0 && word(m, 0x2014) == 13, "CPU program sees the load delay of one instruction");
    CHECK(word(m, 0x2018) == 14 && word(m, 0x201c) == 0, "CPU program runs the branch delay slot and skips the fall-through");
    CHECK(word(m, 0x2020) == 35, "CPU program's product goes through registers and memory");

    ps1_reset(m); ncode = 0;
    li(8, 0x1000); emit(0x48c80000); emit(0x48c81000); emit(0x48c82000);   // CTC2: identity R11, R22, R33
    li(8, 256); emit(0x48c8d000);   // H = 256
    li(8, 20u << 16 | 10); emit(0x48880000); li(8, 512); emit(0x48880800);   // V0 = (10, 20, 512)
    emit(0x4a080001); emit(0x480a7000); emit(0); store(10, 0x2100);   // RTPS sf=1; MFC2 SXY2 and its load delay
    emit(0x480b9800); emit(0); store(11, 0x2104);   // SZ3 = 512
    li(8, 0); emit(0x48880800); emit(0x4a080001); emit(0x484cf800); emit(0); store(12, 0x2108);
    emit(0x49000002); emit(ins(9, 0, 20, 17)); emit(ins(9, 0, 21, 99));   // BC2F taken; its delay slot runs
    emit(0x49010002); emit(ins(9, 0, 22, 23)); emit(ins(9, 0, 23, 29));   // BC2T not taken; its delay slot runs too
    store(20, 0x2110); store(21, 0x2114); store(22, 0x2118); store(23, 0x211c); finish();
    CHECK(run(m), "GTE program reaches its success exit port");
    CHECK(word(m, 0x2100) == (10u << 16 | 5) && word(m, 0x2104) == 512, "GTE program projects a vector with H/Z = 1/2");
    CHECK(word(m, 0x2108) & 1u << 17, "GTE program sees the division overflow at depth 0");
    CHECK(word(m, 0x2110) == 17 && word(m, 0x2114) == 0 && word(m, 0x2118) == 23 && word(m, 0x211c) == 29,
          "COP2 branches see their condition tied low, and both delay slots run");

    ps1_reset(m); ncode = 0; li(8, 0x1f801810);
    gp0(0xe3000000); gp0(0xe4000000 | 1023 | 511 << 10); gp0(0xe1000100);
    gp0(0xa0000000); gp0(0); gp0(0x00010001); gp0(0x1f);
    gp0(0x6d000000); gp0(64u << 16 | 64); gp0(0);
    gp0(0xc0000000); gp0(64u << 16 | 64); gp0(0x00010001);
    emit(ins(35, 8, 10, 0)); emit(0); store(10, 0x2200); finish();
    CHECK(run(m), "GPU program reaches its success exit port");
    CHECK(word(m, 0x2200) == 31 && m->gpu.vram[64 * VRAM_W + 64] == 31, "GPU program uploads a page, draws a textured sprite and reads the framebuffer back");
    CHECK(!m->red, "and no program met a gap of the runtime ([RED])");

    printf("homebrew: %d checks, %d failed\n", checks, fails);
    ps1_free(m);
    return fails != 0;
}
