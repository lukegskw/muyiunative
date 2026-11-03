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
enum { REGIMM = 1, BEQ = 4, ADDIU = 9, ORI = 13, LUI = 15, COP0 = 16, COP1 = 17, COP2 = 18, COP3 = 19, LWL = 34, LW = 35, LWR = 38, SWL = 42, SW = 43, SWR = 46, SWC1 = 57, SWC3 = 59 };
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
    // The hardware log of ps1-tests cpu/cop: with its coprocessor enabled, an instruction of COP1/COP3, an LWCn/SWCn, or a COP0 TLB command raises nothing
    RUN(I(LUI, 0, 1, 0xa000), I(COP0, 4, 1, C0_SR << 11), I(COP1, 0, 0, 0), I(COP3, 0, 0, 0), I(SWC3, 0, 1, 0), I(COP0, 0x1f, 0, 0), NOP);
    CHECK(m->cpu.pc == CODE + 28 && !(m->cpu.cop0[C0_CAUSE] >> 2 & 31), "COP1, COP3, SWC3 and a COP0 TLB command, with SR.CU1/CU3 on: no exception");
    RUN(I(SWC1, 0, 1, 0));
    CHECK(m->cpu.pc == VECTOR && (m->cpu.cop0[C0_CAUSE] >> 2 & 31) == EXC_CPU && (m->cpu.cop0[C0_CAUSE] >> 28 & 3) == 1, "SWC1 while SR.CU1 is off: coprocessor 1 unusable");

    RUN(I(ADDIU, 0, 1, 0x3c), I(COP0, 4, 1, C0_SR << 11), RFE);
    CHECK((m->cpu.cop0[C0_SR] & 0x3f) == 0x3f, "RFE pops the interrupt-enable/kernel-mode stack (the old pair stays)");

    // An enabled IRQ waits for the end of the block, after a branch's delay slot (plan.md, decision log: interrupts at the end of a block); EPC is where the next
    // block starts. The handler here is a loop, and cpu_run stops at the end of a block once the next event is due
    mem_write32(m, VECTOR, I(BEQ, 0, 0, -1)); mem_write32(m, VECTOR + 4, NOP);
    RUN_STEPS(1, NOP, I(ADDIU, 0, 1, 1), I(BEQ, 0, 0, 2), I(ADDIU, 0, 2, 2), I(ADDIU, 0, 3, 3), I(ADDIU, 0, 4, 4));
    m->cpu.cop0[C0_SR] = 0x401; m->i_mask = 1u << IRQ_VBLANK; irq_raise(m, IRQ_VBLANK);
    m->next_event = m->cycles + 20; cpu_run(m);
    CHECK((m->cpu.cop0[C0_CAUSE] >> 2 & 31) == EXC_INT && m->cpu.cop0[C0_EPC] == CODE + 20 && (m->cpu.cop0[C0_SR] & 0x3f) == 4 && (m->cpu.pc & ~4u) == VECTOR,
          "an enabled IRQ is taken at the end of the block, which EPC points after; IEc moves to IEp");
    CHECK(r[1] == 1 && r[2] == 2 && r[3] == 0 && r[4] == 0, "the IRQ waits for the block's branch and its delay slot");
    m->i_stat = 0; irq_update(m);
    CHECK(!(m->cpu.cop0[C0_CAUSE] & 0x400), "acknowledging the IRQ in I_STAT clears CAUSE.IP2");

    // psx-spx, COP0 Exception Handling / Interrupts vs GTE Commands: the command at EPC executes even though the IRQ handler starts there. A BIOS-style handler
    // skips it on return. RGB FIFO movement distinguishes one execution from a skipped or repeated GPF; transfers and disabled COP2 must have no such side effect.
    const uint32_t irq_ops[] = {0x4a08003d, I(COP2, 0, 8, 12 << 11), I(COP2, 4, 8, 15 << 11), 0x4a08003d};
    for (unsigned k = 0; k < sizeof irq_ops / sizeof *irq_ops; k++) {
        RUN_STEPS(0, irq_ops[k]);
        m->cpu.cop0[C0_SR] = 0x401 | (k == 3 ? 0 : 1u << 30);
        m->cpu.cop2d[8] = 4096; m->cpu.cop2d[9] = 16; m->cpu.cop2d[10] = 32; m->cpu.cop2d[11] = 48;
        m->cpu.cop2d[20] = 0x112233; m->cpu.cop2d[21] = 0x445566; m->cpu.cop2d[22] = 0x778899;
        m->cpu.ld_r = 7; m->cpu.ld_v = 0x12345678;
        m->i_mask = 1u << IRQ_VBLANK; irq_raise(m, IRQ_VBLANK);
        m->next_event = m->cycles + 20; cpu_run(m);
        CHECK(m->cpu.cop0[C0_EPC] == CODE && !(m->cpu.cop0[C0_CAUSE] & 0x8000007c) && (m->cpu.pc & ~4u) == VECTOR,
              "an IRQ on a COP2 opcode preserves EPC and enters the handler without BD");
        CHECK(m->cpu.cop2d[20] == (k ? 0x112233u : 0x445566u) && m->cpu.cop2d[21] == (k ? 0x445566u : 0x778899u) &&
              m->cpu.cop2d[22] == (k ? 0x778899u : 0x030201u), "only an enabled GTE command executes once before IRQ entry");
        CHECK(r[7] == 0x12345678 && !m->cpu.ld_r && !r[8], "IRQ entry commits the preceding load without executing a COP2 transfer");
        m->i_stat = 0; irq_update(m);
    }

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
