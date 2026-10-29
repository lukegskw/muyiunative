// COP0, the R3000A's system control coprocessor (psx-spx, CPU Specifications: COP0 Register Summary, COP0 Exception Handling): the status register with its three-level
// interrupt-enable/kernel-mode stack (pushed on an exception, popped by RFE), CAUSE, EPC, and the exception vector (80000080h, or BFC00180h while SR.BEV is set)
#include "ps1.h"

void cpu_reset(ps1 *m) {
    ps1_cpu *c = &m->cpu;
    *c = (ps1_cpu){0};
    c->pc = 0xbfc00000; c->next_pc = c->pc + 4;
    c->cop0[C0_SR] = 1u << 22;   // BEV: exceptions vector into the BIOS until it sets up RAM
    c->cop0[C0_PRID] = 2;        // R3000A
    irq_update(m);
}

// EPC is the faulting instruction, or the branch before it when it sits in a delay slot (CAUSE.BD), so that returning runs the branch again
void cpu_exception(ps1 *m, uint32_t code) {
    ps1_cpu *c = &m->cpu;
    if (c->ld_r) { c->r[c->ld_r] = c->ld_v; c->ld_r = 0; }   // the load in flight still lands before the handler runs
    uint32_t sr = c->cop0[C0_SR];
    c->cop0[C0_SR] = (sr & ~0x3fu) | (sr << 2 & 0x3f);
    c->cop0[C0_CAUSE] = (c->cop0[C0_CAUSE] & 0xff00) | code << 2 | (c->delay ? 0x80000000u : 0);
    c->cop0[C0_EPC] = c->delay ? c->cur_pc - 4 : c->cur_pc;
    c->pc = sr & 1u << 22 ? 0xbfc00180 : 0x80000080;
    c->next_pc = c->pc + 4;
    c->branch = 0;
}
