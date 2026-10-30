// The R3000A's instructions, one inline function each (psx-spx, CPU Specifications; the MIPS I instruction set), shared by the interpreter (interp.c) and the native
// code the tools generate (plan.md, phase 3), so that both run the same definitions; the names are those of tools/mips.py, whose decoder matches cpu_decode here.
// cpu_begin starts an instruction: its fetch's cycles, the PC moving on (to a branch's target after its delay slot), and the load in flight from the instruction
// before (cpu_ld), which lands at cpu_end unless the instruction writes that register itself (the load delay slot). An instruction function returns 1 when it raised
// an exception, the CPU then being at the exception vector. An instruction in a branch's delay slot ends a block, as an exception does (cpu.boundary): the interpreter
// and native code take events and interrupts only there (plan.md, decision log: interrupts at the end of a block)
#pragma once
#include "ps1.h"

#define CPU_OPS(X) X(sll) X(srl) X(sra) X(sllv) X(srlv) X(srav) X(jr) X(jalr) X(syscall) X(break) X(mfhi) X(mthi) X(mflo) X(mtlo) X(mult) X(multu) X(div) X(divu) \
    X(add) X(addu) X(sub) X(subu) X(and) X(or) X(xor) X(nor) X(slt) X(sltu) X(bltz) X(bgez) X(bltzal) X(bgezal) X(j) X(jal) X(beq) X(bne) X(blez) X(bgtz) X(addi) \
    X(addiu) X(slti) X(sltiu) X(andi) X(ori) X(xori) X(lui) X(mfc0) X(mtc0) X(rfe) X(cop0) X(cop2) X(mfc2) X(cfc2) X(mtc2) X(ctc2) X(bc2f) X(bc2t) X(cop) X(lb) X(lh) \
    X(lwl) X(lw) X(lbu) X(lhu) X(lwr) X(sb) X(sh) X(swl) X(sw) X(swr) X(lwc2) X(swc2) X(ri)
enum {
#define X(n) CPU_##n,
    CPU_OPS(X)
#undef X
    CPU_NOPS
};

#define CPU_RS ((op) >> 21 & 31)
#define CPU_RT ((op) >> 16 & 31)
#define CPU_RD ((op) >> 11 & 31)
#define CPU_SA ((op) >> 6 & 31)
#define CPU_SIMM ((uint32_t)(int32_t)(int16_t)(op))
#define CPU_ZIMM ((op) & 0xffff)
#define CPU_INLINE static inline __attribute__((always_inline))

// What a word is, as tools/mips.py decodes it: SPECIAL by its function field, BcondZ by bits 0 and 4 of rt, COP0 and COP2 by rs, RFE by its function field with bit 25
// set; every encoding not listed is a reserved instruction (ri)
CPU_INLINE int cpu_decode(uint32_t op) {
    static const uint8_t special[64] = {
        CPU_sll, CPU_ri, CPU_srl, CPU_sra, CPU_sllv, CPU_ri, CPU_srlv, CPU_srav, CPU_jr, CPU_jalr, CPU_ri, CPU_ri, CPU_syscall, CPU_break, CPU_ri, CPU_ri,
        CPU_mfhi, CPU_mthi, CPU_mflo, CPU_mtlo, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_mult, CPU_multu, CPU_div, CPU_divu, CPU_ri, CPU_ri, CPU_ri, CPU_ri,
        CPU_add, CPU_addu, CPU_sub, CPU_subu, CPU_and, CPU_or, CPU_xor, CPU_nor, CPU_ri, CPU_ri, CPU_slt, CPU_sltu, CPU_ri, CPU_ri, CPU_ri, CPU_ri,
        CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri};
    static const uint8_t primary[64] = {
        CPU_ri, CPU_ri, CPU_j, CPU_jal, CPU_beq, CPU_bne, CPU_blez, CPU_bgtz, CPU_addi, CPU_addiu, CPU_slti, CPU_sltiu, CPU_andi, CPU_ori, CPU_xori, CPU_lui,
        CPU_ri, CPU_cop, CPU_ri, CPU_cop, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_ri,
        CPU_lb, CPU_lh, CPU_lwl, CPU_lw, CPU_lbu, CPU_lhu, CPU_lwr, CPU_ri, CPU_sb, CPU_sh, CPU_swl, CPU_sw, CPU_ri, CPU_ri, CPU_swr, CPU_ri,
        CPU_cop, CPU_cop, CPU_lwc2, CPU_cop, CPU_ri, CPU_ri, CPU_ri, CPU_ri, CPU_cop, CPU_cop, CPU_swc2, CPU_cop, CPU_ri, CPU_ri, CPU_ri, CPU_ri};
    switch (op >> 26) {
    case 0x00: return special[op & 0x3f];
    case 0x01: return (CPU_RT & 0x1e) == 0x10 ? (CPU_RT & 1 ? CPU_bgezal : CPU_bltzal) : (CPU_RT & 1 ? CPU_bgez : CPU_bltz);
    case 0x10:
        if (CPU_RS == 0) return CPU_mfc0;
        if (CPU_RS == 4) return CPU_mtc0;
        return op >> 25 & 1 ? ((op & 0x3f) == 0x10 ? CPU_rfe : CPU_cop0) : CPU_ri;
    case 0x12:
        if (op >> 25 & 1) return CPU_cop2;
        switch (CPU_RS) {
        case 0: return CPU_mfc2;
        case 2: return CPU_cfc2;
        case 4: return CPU_mtc2;
        case 6: return CPU_ctc2;
        case 8: return CPU_RT == 0 ? CPU_bc2f : CPU_RT == 1 ? CPU_bc2t : CPU_ri;
        default: return CPU_ri;
        }
    default: return primary[op >> 26];
    }
}

typedef struct cpu_ld { uint32_t r, v; } cpu_ld;   // the load in flight from the instruction before: register and value

CPU_INLINE cpu_ld cpu_begin(ps1 *m, uint32_t pc, unsigned fetch) {   // the instruction at pc, fetched in `fetch` cycles
    ps1_cpu *c = &m->cpu;
    c->delay = c->branch; c->branch = 0; c->cur_pc = pc; c->boundary = c->delay;
    m->cycles += fetch;
    c->pc = c->next_pc; c->next_pc += 4;
    cpu_ld l = {c->ld_r, c->ld_v};
    c->ld_r = 0;
    return l;
}
CPU_INLINE void cpu_end(ps1 *m, cpu_ld l) { if (l.r) m->cpu.r[l.r] = l.v; }

// An interrupt is due at the end of a block, when the CPU has interrupts enabled (SR.IEc) and one of them pending in CAUSE and unmasked in SR. Not while a delay slot
// is next: a branch in a delay slot leaves its own delay slot to run
CPU_INLINE int cpu_irq_due(const ps1 *m) {
    const ps1_cpu *c = &m->cpu;
    return c->boundary && !c->branch && (c->cop0[C0_SR] & 1) && (c->cop0[C0_SR] & c->cop0[C0_CAUSE] & 0xff00);
}

CPU_INLINE void cpu_w(ps1 *m, cpu_ld *l, uint32_t d, uint32_t x) { if (d) { m->cpu.r[d] = x; if (d == l->r) l->r = 0; } }   // a write wins over the load in flight
CPU_INLINE void cpu_load(ps1 *m, uint32_t d, uint32_t x) { if (d) { m->cpu.ld_r = d; m->cpu.ld_v = x; } }   // lands after the next instruction
CPU_INLINE void cpu_branch(ps1 *m, int taken, uint32_t op) { ps1_cpu *c = &m->cpu; if (taken) c->next_pc = c->pc + (CPU_SIMM << 2); c->branch = 1; }
CPU_INLINE void cpu_wait(ps1 *m, uint32_t a, unsigned n) { m->cycles += mem_load_cycles(a, n) - 1; }   // before the read, so a device sees the time its data leaves
CPU_INLINE void cpu_hilo(ps1 *m, unsigned fetch) {   // MFHI/MFLO wait for MULT/DIV's result
    if (m->cycles - fetch < m->cpu.hilo_busy) m->cycles = m->cpu.hilo_busy + fetch;
}
CPU_INLINE int cpu_addr_error(ps1 *m, uint32_t a, uint32_t code) { m->cpu.cop0[C0_BADVADDR] = a; cpu_exception(m, code); return 1; }
CPU_INLINE int cpu_unusable(ps1 *m, uint32_t cop) { cpu_exception(m, EXC_CPU); m->cpu.cop0[C0_CAUSE] |= cop << 28; return 1; }
CPU_INLINE int cpu_isolated(const ps1 *m) { return m->cpu.cop0[C0_SR] & 0x10000; }   // SR.IsC: stores reach the cache, not memory (the BIOS's cache flush)
CPU_INLINE int cpu_cu2(const ps1 *m) { return m->cpu.cop0[C0_SR] >> 30 & 1; }
// COP1 and COP3 do not exist, and COP0 has no TLB, yet with the coprocessor enabled in SR the hardware raises no exception for their instructions, LWCn/SWCn, or the TLB
// commands (the hardware log of ps1-tests cpu/cop); what else they do is not known, so here they do nothing
CPU_INLINE void cpu_no_effect(ps1 *m, uint32_t op) { RED_ONCE(m, RED_COP0, "cpu: coprocessor instruction %08x at %08x has no documented effect: nothing done", op, m->cpu.cur_pc); }
CPU_INLINE unsigned cpu_mult_cycles(uint32_t rs, int sign) {   // a negative rs counts by its complement: FFFFF800h is as fast as 7FFh
    uint32_t v = sign && (int32_t)rs < 0 ? ~rs : rs;
    return v < 0x800 ? 6 : v < 0x100000 ? 9 : 13;
}

#define CPU_OP(n) CPU_INLINE int op_##n(ps1 *m, cpu_ld *l, uint32_t op, unsigned fetch)
#define S (m->cpu.r[CPU_RS])
#define T (m->cpu.r[CPU_RT])
#define W(d, x) cpu_w(m, l, (d), (x))
#define UNUSED (void)l, (void)op, (void)fetch
CPU_OP(sll) { UNUSED; W(CPU_RD, T << CPU_SA); return 0; }
CPU_OP(srl) { UNUSED; W(CPU_RD, T >> CPU_SA); return 0; }
CPU_OP(sra) { UNUSED; W(CPU_RD, (uint32_t)((int32_t)T >> CPU_SA)); return 0; }
CPU_OP(sllv) { UNUSED; W(CPU_RD, T << (S & 31)); return 0; }
CPU_OP(srlv) { UNUSED; W(CPU_RD, T >> (S & 31)); return 0; }
CPU_OP(srav) { UNUSED; W(CPU_RD, (uint32_t)((int32_t)T >> (S & 31))); return 0; }
CPU_OP(jr) { UNUSED; m->cpu.next_pc = S; m->cpu.branch = 1; return 0; }
CPU_OP(jalr) { UNUSED; uint32_t s = S; W(CPU_RD, m->cpu.pc + 4); m->cpu.next_pc = s; m->cpu.branch = 1; return 0; }
CPU_OP(syscall) { UNUSED; cpu_exception(m, EXC_SYS); return 1; }
CPU_OP(break) { UNUSED; cpu_exception(m, EXC_BP); return 1; }
CPU_OP(mfhi) { UNUSED; cpu_hilo(m, fetch); W(CPU_RD, m->cpu.hi); return 0; }
CPU_OP(mthi) { UNUSED; m->cpu.hi = S; return 0; }
CPU_OP(mflo) { UNUSED; cpu_hilo(m, fetch); W(CPU_RD, m->cpu.lo); return 0; }
CPU_OP(mtlo) { UNUSED; m->cpu.lo = S; return 0; }
CPU_OP(mult) {
    UNUSED; uint32_t s = S; int64_t p = (int64_t)(int32_t)s * (int32_t)T;
    m->cpu.lo = (uint32_t)p; m->cpu.hi = (uint32_t)((uint64_t)p >> 32); m->cpu.hilo_busy = m->cycles + cpu_mult_cycles(s, 1); return 0;
}
CPU_OP(multu) {
    UNUSED; uint32_t s = S; uint64_t p = (uint64_t)s * T;
    m->cpu.lo = (uint32_t)p; m->cpu.hi = (uint32_t)(p >> 32); m->cpu.hilo_busy = m->cycles + cpu_mult_cycles(s, 0); return 0;
}
CPU_OP(div) {   // division by zero and the one overflowing case give fixed results (psx-spx, CPU Arithmetic Opcodes)
    UNUSED; uint32_t s = S, t = T; ps1_cpu *c = &m->cpu;
    if (!t) { c->hi = s; c->lo = (int32_t)s < 0 ? 1 : 0xffffffff; }
    else if (s == 0x80000000 && t == 0xffffffff) { c->hi = 0; c->lo = 0x80000000; }
    else { c->lo = (uint32_t)((int32_t)s / (int32_t)t); c->hi = (uint32_t)((int32_t)s % (int32_t)t); }
    c->hilo_busy = m->cycles + 36; return 0;
}
CPU_OP(divu) {
    UNUSED; uint32_t s = S, t = T; ps1_cpu *c = &m->cpu;
    if (!t) { c->hi = s; c->lo = 0xffffffff; } else { c->lo = s / t; c->hi = s % t; }
    c->hilo_busy = m->cycles + 36; return 0;
}
CPU_OP(add) { UNUSED; int32_t r; if (__builtin_add_overflow((int32_t)S, (int32_t)T, &r)) { cpu_exception(m, EXC_OV); return 1; } W(CPU_RD, (uint32_t)r); return 0; }
CPU_OP(addu) { UNUSED; W(CPU_RD, S + T); return 0; }
CPU_OP(sub) { UNUSED; int32_t r; if (__builtin_sub_overflow((int32_t)S, (int32_t)T, &r)) { cpu_exception(m, EXC_OV); return 1; } W(CPU_RD, (uint32_t)r); return 0; }
CPU_OP(subu) { UNUSED; W(CPU_RD, S - T); return 0; }
CPU_OP(and) { UNUSED; W(CPU_RD, S & T); return 0; }
CPU_OP(or) { UNUSED; W(CPU_RD, S | T); return 0; }
CPU_OP(xor) { UNUSED; W(CPU_RD, S ^ T); return 0; }
CPU_OP(nor) { UNUSED; W(CPU_RD, ~(S | T)); return 0; }
CPU_OP(slt) { UNUSED; W(CPU_RD, (int32_t)S < (int32_t)T); return 0; }
CPU_OP(sltu) { UNUSED; W(CPU_RD, S < T); return 0; }
// BcondZ: bit 0 of rt picks GEZ or LTZ; rt 10h/11h (and their mirrors) link, taken or not (psx-spx, CPU Jump Opcodes)
CPU_OP(bltz) { UNUSED; cpu_branch(m, (int32_t)S < 0, op); return 0; }
CPU_OP(bgez) { UNUSED; cpu_branch(m, (int32_t)S >= 0, op); return 0; }
CPU_OP(bltzal) { UNUSED; uint32_t s = S; W(31, m->cpu.pc + 4); cpu_branch(m, (int32_t)s < 0, op); return 0; }
CPU_OP(bgezal) { UNUSED; uint32_t s = S; W(31, m->cpu.pc + 4); cpu_branch(m, (int32_t)s >= 0, op); return 0; }
CPU_OP(j) { UNUSED; m->cpu.next_pc = (m->cpu.pc & 0xf0000000) | (op & 0x3ffffff) << 2; m->cpu.branch = 1; return 0; }
CPU_OP(jal) { UNUSED; W(31, m->cpu.pc + 4); m->cpu.next_pc = (m->cpu.pc & 0xf0000000) | (op & 0x3ffffff) << 2; m->cpu.branch = 1; return 0; }
CPU_OP(beq) { UNUSED; cpu_branch(m, S == T, op); return 0; }
CPU_OP(bne) { UNUSED; cpu_branch(m, S != T, op); return 0; }
CPU_OP(blez) { UNUSED; cpu_branch(m, (int32_t)S <= 0, op); return 0; }
CPU_OP(bgtz) { UNUSED; cpu_branch(m, (int32_t)S > 0, op); return 0; }
CPU_OP(addi) { UNUSED; int32_t r; if (__builtin_add_overflow((int32_t)S, (int32_t)CPU_SIMM, &r)) { cpu_exception(m, EXC_OV); return 1; } W(CPU_RT, (uint32_t)r); return 0; }
CPU_OP(addiu) { UNUSED; W(CPU_RT, S + CPU_SIMM); return 0; }
CPU_OP(slti) { UNUSED; W(CPU_RT, (int32_t)S < (int32_t)CPU_SIMM); return 0; }
CPU_OP(sltiu) { UNUSED; W(CPU_RT, S < CPU_SIMM); return 0; }
CPU_OP(andi) { UNUSED; W(CPU_RT, S & CPU_ZIMM); return 0; }
CPU_OP(ori) { UNUSED; W(CPU_RT, S | CPU_ZIMM); return 0; }
CPU_OP(xori) { UNUSED; W(CPU_RT, S ^ CPU_ZIMM); return 0; }
CPU_OP(lui) { UNUSED; W(CPU_RT, CPU_ZIMM << 16); return 0; }
CPU_OP(mfc0) { UNUSED; cpu_load(m, CPU_RT, m->cpu.cop0[CPU_RD]); return 0; }
CPU_OP(mtc0) {
    UNUSED; uint32_t *c0 = m->cpu.cop0, r = CPU_RD, v = T;
    if (r == C0_CAUSE) c0[r] = (c0[r] & ~0x300u) | (v & 0x300);   // only the two software interrupt bits are writable
    else if (r != C0_BADVADDR && r != C0_EPC && r != C0_PRID) c0[r] = v;
    return 0;
}
CPU_OP(rfe) { UNUSED; uint32_t *sr = &m->cpu.cop0[C0_SR]; *sr = (*sr & ~0xfu) | (*sr >> 2 & 0xf); return 0; }
CPU_OP(cop0) { UNUSED; cpu_no_effect(m, op); return 0; }
CPU_OP(cop2) { UNUSED; if (!cpu_cu2(m)) return cpu_unusable(m, 2); gte_command(m, op); return 0; }
CPU_OP(mfc2) { UNUSED; if (!cpu_cu2(m)) return cpu_unusable(m, 2); cpu_load(m, CPU_RT, gte_read(m, CPU_RD, 0)); return 0; }
CPU_OP(cfc2) { UNUSED; if (!cpu_cu2(m)) return cpu_unusable(m, 2); cpu_load(m, CPU_RT, gte_read(m, CPU_RD, 1)); return 0; }
CPU_OP(mtc2) { UNUSED; if (!cpu_cu2(m)) return cpu_unusable(m, 2); gte_write(m, CPU_RD, T, 0); return 0; }
CPU_OP(ctc2) { UNUSED; if (!cpu_cu2(m)) return cpu_unusable(m, 2); gte_write(m, CPU_RD, T, 1); return 0; }
CPU_OP(bc2f) { UNUSED; if (!cpu_cu2(m)) return cpu_unusable(m, 2); cpu_branch(m, 1, op); return 0; }   // psx-spx, GTE Opcodes: COP2's condition input is tied low
CPU_OP(bc2t) { UNUSED; if (!cpu_cu2(m)) return cpu_unusable(m, 2); cpu_branch(m, 0, op); return 0; }
CPU_OP(cop) { UNUSED; if (m->cpu.cop0[C0_SR] >> (28 + (op >> 26 & 3)) & 1) { cpu_no_effect(m, op); return 0; } return cpu_unusable(m, op >> 26 & 3); }
CPU_OP(lb) { UNUSED; uint32_t a = S + CPU_SIMM; cpu_wait(m, a, 1); cpu_load(m, CPU_RT, (uint32_t)(int8_t)mem_read8(m, a)); return 0; }
CPU_OP(lh) { UNUSED; uint32_t a = S + CPU_SIMM; if (a & 1) return cpu_addr_error(m, a, EXC_ADEL); cpu_wait(m, a, 2); cpu_load(m, CPU_RT, (uint32_t)(int16_t)mem_read16(m, a)); return 0; }
CPU_OP(lw) { UNUSED; uint32_t a = S + CPU_SIMM; if (a & 3) return cpu_addr_error(m, a, EXC_ADEL); cpu_wait(m, a, 4); cpu_load(m, CPU_RT, mem_read32(m, a)); return 0; }
CPU_OP(lbu) { UNUSED; uint32_t a = S + CPU_SIMM; cpu_wait(m, a, 1); cpu_load(m, CPU_RT, mem_read8(m, a)); return 0; }
CPU_OP(lhu) { UNUSED; uint32_t a = S + CPU_SIMM; if (a & 1) return cpu_addr_error(m, a, EXC_ADEL); cpu_wait(m, a, 2); cpu_load(m, CPU_RT, mem_read16(m, a)); return 0; }
// LWL / LWR: the bytes of an unaligned word on each side of a word boundary, merged into rt, or into the load in flight to rt
CPU_OP(lwl) {
    UNUSED; uint32_t a = S + CPU_SIMM; cpu_wait(m, a, 4);
    uint32_t w = mem_read32(m, a & ~3u), cur = l->r == CPU_RT ? l->v : T, sh = (a & 3) * 8;
    cpu_load(m, CPU_RT, (cur & (0x00ffffffu >> sh)) | w << (24 - sh)); return 0;
}
CPU_OP(lwr) {
    UNUSED; uint32_t a = S + CPU_SIMM; cpu_wait(m, a, 4);
    uint32_t w = mem_read32(m, a & ~3u), cur = l->r == CPU_RT ? l->v : T, sh = (a & 3) * 8;
    cpu_load(m, CPU_RT, (cur & ~(0xffffffffu >> sh)) | w >> sh); return 0;
}
CPU_OP(sb) { UNUSED; uint32_t a = S + CPU_SIMM; if (!cpu_isolated(m)) mem_write8(m, a, T); return 0; }
CPU_OP(sh) { UNUSED; uint32_t a = S + CPU_SIMM; if (a & 1) return cpu_addr_error(m, a, EXC_ADES); if (!cpu_isolated(m)) mem_write16(m, a, T); return 0; }
CPU_OP(sw) { UNUSED; uint32_t a = S + CPU_SIMM; if (a & 3) return cpu_addr_error(m, a, EXC_ADES); if (!cpu_isolated(m)) mem_write32(m, a, T); return 0; }
CPU_OP(swl) {
    UNUSED; uint32_t a = S + CPU_SIMM, t = T, sh = (a & 3) * 8;
    if (cpu_isolated(m)) return 0;
    uint32_t w = mem_read32(m, a & ~3u);
    mem_write32(m, a & ~3u, (w & ~(0xffffffffu >> (24 - sh))) | t >> (24 - sh)); return 0;
}
CPU_OP(swr) {
    UNUSED; uint32_t a = S + CPU_SIMM, t = T, sh = (a & 3) * 8;
    if (cpu_isolated(m)) return 0;
    uint32_t w = mem_read32(m, a & ~3u);
    mem_write32(m, a & ~3u, (w & ~(0xffffffffu << sh)) | t << sh); return 0;
}
CPU_OP(lwc2) {
    UNUSED; uint32_t a = S + CPU_SIMM;
    if (!cpu_cu2(m)) return cpu_unusable(m, 2);
    if (a & 3) return cpu_addr_error(m, a, EXC_ADEL);
    cpu_wait(m, a, 4); gte_write(m, CPU_RT, mem_read32(m, a), 0); return 0;
}
CPU_OP(swc2) {
    UNUSED; uint32_t a = S + CPU_SIMM;
    if (!cpu_cu2(m)) return cpu_unusable(m, 2);
    if (a & 3) return cpu_addr_error(m, a, EXC_ADES);
    if (!cpu_isolated(m)) mem_write32(m, a, gte_read(m, CPU_RT, 0));
    return 0;
}
CPU_OP(ri) {   // with COP2 disabled, its reserved encodings are unusable first
    UNUSED;
    if (op >> 26 == 0x12 && !cpu_cu2(m)) return cpu_unusable(m, 2);
    if (op >> 26 == 0x10) RED_ONCE(m, RED_COP0, "cpu: COP0 instruction %08x at %08x does not exist on the R3000A", op, m->cpu.cur_pc);
    cpu_exception(m, EXC_RI); return 1;
}
#undef S
#undef T
#undef W
#undef UNUSED
#undef CPU_OP

// One instruction, decoded at run time: the interpreter's
CPU_INLINE int cpu_exec(ps1 *m, uint32_t op, unsigned fetch) {
    cpu_ld l = cpu_begin(m, m->cpu.pc, fetch);
    int x = 0;
    switch (cpu_decode(op)) {
#define X(n) case CPU_##n: x = op_##n(m, &l, op, fetch); break;
    CPU_OPS(X)
#undef X
    }
    cpu_end(m, l);
    return x;
}
