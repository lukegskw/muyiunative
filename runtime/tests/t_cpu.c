// The interpreter (interp.c, cpu.c) against the R3000A (psx-spx, CPU Specifications): arithmetic, the branch delay slot, the load delay slot (and a load that the next
// instruction's write to the same register cancels), LWL/LWR merging with a load in flight, SWL/SWR, division by zero and its overflow case, the exceptions (overflow,
// address error, syscall in a delay slot: EPC and CAUSE.BD, the unusable coprocessor), RFE, interrupts, links that BLTZAL makes when not taken, and cache isolation.
// The programs are assembled here and run from RAM
#include "ps1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

#define R(fn, rs, rt, rd, sa) ((uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | (uint32_t)(rd) << 11 | (uint32_t)(sa) << 6 | (uint32_t)(fn))
#define I(op, rs, rt, imm) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((uint32_t)(imm) & 0xffff))
#define JAL(a) (3u << 26 | ((uint32_t)(a) >> 2 & 0x3ffffff))
enum { REGIMM = 1, BEQ = 4, ADDIU = 9, ORI = 13, LUI = 15, COP0 = 16, COP2 = 18, LWL = 34, LW = 35, LWR = 38, SWL = 42, SW = 43, SWR = 46 };
enum { SRA = 3, SYSCALL = 12, MULT = 24, DIV = 26, DIVU = 27, ADD = 32, ADDU = 33, SLT = 42, SLTU = 43 };
enum { CODE = 0x80001000, DATA = 0x80002000, VECTOR = 0x80000080 };
#define NOP 0u
#define RFE 0x42000010u
#define LA8 I(LUI, 0, 8, DATA >> 16), I(ORI, 8, 8, DATA & 0xffff)   // r8 = DATA

static ps1 *m;
#define RUN(...) do { const uint32_t p_[] = {__VA_ARGS__}; int n_ = (int)(sizeof p_ / sizeof *p_); run(p_, n_, n_); } while (0)
#define RUN_STEPS(k, ...) do { const uint32_t p_[] = {__VA_ARGS__}; run(p_, (int)(sizeof p_ / sizeof *p_), k); } while (0)
static void run(const uint32_t *code, int n, int steps) {   // a fresh CPU at CODE with SR = 0 (exceptions go to 80000080h), then the steps
    for (int i = 0; i < n; i++) mem_write32(m, CODE + 4 * (uint32_t)i, code[i]);
    cpu_reset(m); m->cpu.cop0[C0_SR] = 0; m->cpu.pc = CODE; m->cpu.next_pc = CODE + 4;
    while (steps--) cpu_step(m);
}
static uint32_t *r = NULL;

int main(void) {
    m = ps1_new();
    if (!m) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }
    r = m->cpu.r;

    RUN(I(LUI, 0, 1, 0x1234), I(ORI, 1, 1, 0x5678), I(ADDIU, 0, 2, -1), R(ADDU, 1, 2, 3, 0), R(SLT, 2, 0, 4, 0), R(SLTU, 2, 0, 5, 0), R(SRA, 0, 2, 6, 4));
    CHECK(r[1] == 0x12345678 && r[2] == 0xffffffff && r[3] == 0x12345677 && r[4] == 1 && r[5] == 0 && r[6] == 0xffffffff, "LUI, ORI, ADDIU, ADDU, SLT, SLTU, SRA");
    RUN(I(ADDIU, 0, 0, 5));
    CHECK(r[0] == 0, "r0 stays 0");

    RUN_STEPS(3, I(BEQ, 0, 0, 2), I(ADDIU, 0, 1, 1), I(ADDIU, 0, 2, 2), I(ADDIU, 0, 3, 3));
    CHECK(r[1] == 1 && r[2] == 0 && r[3] == 3 && m->cpu.pc == CODE + 16, "a taken branch runs its delay slot, then its target");

    mem_write32(m, DATA, 0xdeadbeef);
    RUN(LA8, I(ADDIU, 0, 1, 7), I(LW, 8, 1, 0), R(ADDU, 1, 0, 2, 0), R(ADDU, 1, 0, 3, 0));
    CHECK(r[2] == 7 && r[3] == 0xdeadbeef, "the instruction after a load sees the register's old value");
    RUN(LA8, I(LW, 8, 1, 0), I(ADDIU, 0, 1, 5), NOP);
    CHECK(r[1] == 5, "a write in the load delay slot wins over the load");

    mem_write32(m, DATA, 0x33221100); mem_write32(m, DATA + 4, 0x77665544);
    RUN(LA8, I(LUI, 0, 1, 0xaaaa), I(LWR, 8, 1, 1), I(LWL, 8, 1, 4), NOP);
    CHECK(r[1] == 0x44332211, "LWR then LWL load an unaligned word, LWL merging with LWR's load in flight");
    mem_write32(m, DATA + 0x10, 0xffffffff); mem_write32(m, DATA + 0x14, 0xffffffff);
    RUN(LA8, I(LUI, 0, 1, 0x4433), I(ORI, 1, 1, 0x2211), I(SWR, 8, 1, 0x11), I(SWL, 8, 1, 0x14));
    CHECK(mem_read32(m, DATA + 0x10) == 0x332211ff && mem_read32(m, DATA + 0x14) == 0xffffff44, "SWR then SWL store an unaligned word");

    RUN(I(ADDIU, 0, 1, 7), R(DIV, 1, 0, 0, 0));
    CHECK(m->cpu.lo == 0xffffffff && m->cpu.hi == 7, "7 / 0: quotient -1, remainder 7");
    RUN(I(ADDIU, 0, 1, -7), R(DIV, 1, 0, 0, 0));
    CHECK(m->cpu.lo == 1 && m->cpu.hi == 0xfffffff9, "-7 / 0: quotient 1, remainder -7");
    RUN(I(ADDIU, 0, 1, 7), R(DIVU, 1, 0, 0, 0));
    CHECK(m->cpu.lo == 0xffffffff && m->cpu.hi == 7, "DIVU by 0: quotient FFFFFFFFh");
    RUN(I(LUI, 0, 1, 0x8000), I(ADDIU, 0, 2, -1), R(DIV, 1, 2, 0, 0));
    CHECK(m->cpu.lo == 0x80000000 && m->cpu.hi == 0, "80000000h / -1: quotient 80000000h, remainder 0");
    RUN(I(ADDIU, 0, 1, -3), I(ADDIU, 0, 2, 5), R(MULT, 1, 2, 0, 0));
    CHECK(m->cpu.lo == 0xfffffff1 && m->cpu.hi == 0xffffffff, "MULT is signed and 64-bit");

    RUN(I(LUI, 0, 1, 0x7fff), I(ORI, 1, 1, 0xffff), I(ADDIU, 0, 2, 1), R(ADD, 1, 2, 3, 0));
    CHECK(m->cpu.pc == VECTOR && (m->cpu.cop0[C0_CAUSE] >> 2 & 31) == EXC_OV && m->cpu.cop0[C0_EPC] == CODE + 12 && r[3] == 0,
          "ADD overflows: exception 12, EPC at the ADD, no result");
    RUN(I(BEQ, 0, 0, 2), R(SYSCALL, 0, 0, 0, 0));
    CHECK(m->cpu.pc == VECTOR && (m->cpu.cop0[C0_CAUSE] >> 2 & 31) == EXC_SYS && m->cpu.cop0[C0_EPC] == CODE && m->cpu.cop0[C0_CAUSE] >> 31,
          "SYSCALL in a delay slot: EPC at the branch, CAUSE.BD set");
    RUN(LA8, I(LW, 8, 1, 2));
    CHECK(m->cpu.pc == VECTOR && (m->cpu.cop0[C0_CAUSE] >> 2 & 31) == EXC_ADEL && m->cpu.cop0[C0_BADVADDR] == DATA + 2, "an unaligned LW: address error, BADVADDR");
    RUN(I(COP2, 0, 1, 0));
    CHECK(m->cpu.pc == VECTOR && (m->cpu.cop0[C0_CAUSE] >> 2 & 31) == EXC_CPU && (m->cpu.cop0[C0_CAUSE] >> 28 & 3) == 2, "COP2 while SR.CU2 is off: coprocessor 2 unusable");

    RUN(I(ADDIU, 0, 1, 0x3c), I(COP0, 4, 1, C0_SR << 11), RFE);
    CHECK((m->cpu.cop0[C0_SR] & 0x3f) == 0x3f, "RFE pops the interrupt-enable/kernel-mode stack (the old pair stays)");

    RUN(NOP);
    m->cpu.cop0[C0_SR] = 0x401; m->i_mask = 1u << IRQ_VBLANK; irq_raise(m, IRQ_VBLANK);
    uint32_t at = m->cpu.pc; cpu_step(m);
    CHECK(m->cpu.pc == VECTOR && (m->cpu.cop0[C0_CAUSE] >> 2 & 31) == EXC_INT && m->cpu.cop0[C0_EPC] == at && (m->cpu.cop0[C0_SR] & 0x3f) == 4,
          "an enabled IRQ is taken before the next instruction, which EPC points at; IEc moves to IEp");
    m->i_stat = 0; irq_update(m);
    CHECK(!(m->cpu.cop0[C0_CAUSE] & 0x400), "acknowledging the IRQ in I_STAT clears CAUSE.IP2");

    RUN(JAL(CODE + 0x10), NOP);
    CHECK(m->cpu.pc == CODE + 0x10 && r[31] == CODE + 8, "JAL links the address after its delay slot");
    RUN(I(ADDIU, 0, 1, 1), I(REGIMM, 1, 0x10, 4), NOP);
    CHECK(r[31] == CODE + 12 && m->cpu.pc == CODE + 12, "BLTZAL not taken still links");

    mem_write32(m, DATA, 0x11111111);
    RUN(LA8, I(LUI, 0, 1, 0x2222), I(LUI, 0, 2, 1), I(COP0, 4, 2, C0_SR << 11), I(SW, 8, 1, 0));
    CHECK(mem_read32(m, DATA) == 0x11111111, "stores do not reach memory while SR.IsC isolates the cache");

    printf("cpu: %d checks, %d failed\n", checks, fails);
    ps1_free(m);
    return fails != 0;
}
