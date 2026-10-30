// The R3000A interpreter (psx-spx, CPU Specifications; the MIPS I instruction set): one instruction per step. A loaded register reaches the CPU one instruction late (the
// load delay slot: the next instruction still sees the old value, and its own write to that register wins), LWL/LWR merge with a load in flight, and the instruction after
// a branch or jump runs before the branch takes effect (the branch delay slot). Interrupts are taken between instructions. COP2 reaches the GTE, including its register
// side effects and busy interlocks. This interpreter is the reference the recompiled code is checked against (AGENTS.md)
#include "ps1.h"

#define RS (op >> 21 & 31)
#define RT (op >> 16 & 31)
#define RD (op >> 11 & 31)
#define SA (op >> 6 & 31)
#define SIMM ((uint32_t)(int32_t)(int16_t)op)
#define ZIMM (op & 0xffff)

static void cop0_write(ps1 *m, uint32_t r, uint32_t v) {
    uint32_t *c0 = m->cpu.cop0;
    if (r == C0_CAUSE) c0[r] = (c0[r] & ~0x300u) | (v & 0x300);   // only the two software interrupt bits are writable
    else if (r != C0_BADVADDR && r != C0_EPC && r != C0_PRID) c0[r] = v;
}

static void addr_error(ps1 *m, uint32_t a, uint32_t code) { m->cpu.cop0[C0_BADVADDR] = a; cpu_exception(m, code); }
static void unusable(ps1 *m, uint32_t cop) { cpu_exception(m, EXC_CPU); m->cpu.cop0[C0_CAUSE] |= cop << 28; }

void cpu_step(ps1 *m) {
    ps1_cpu *c = &m->cpu;
    uint32_t *c0 = c->cop0, op;
    m->cycles += CPU_CPI;
    c->delay = c->branch; c->branch = 0; c->cur_pc = c->pc;
    if ((c0[C0_SR] & 1) && (c0[C0_SR] & c0[C0_CAUSE] & 0xff00)) { cpu_exception(m, EXC_INT); return; }
    if (c->pc & 3) { addr_error(m, c->pc, EXC_ADEL); return; }
    if (!mem_fetch(m, c->pc, &op)) { cpu_exception(m, EXC_IBE); return; }
    c->pc = c->next_pc; c->next_pc += 4;
    uint32_t ld_r = c->ld_r, ld_v = c->ld_v, s = c->r[RS], t = c->r[RT], a = s + SIMM;   // the previous load lands after this instruction
    c->ld_r = 0;
    int isolated = c0[C0_SR] & 0x10000;   // SR.IsC: stores reach the cache, not memory (the BIOS's cache flush)
#define W(d, x) do { uint32_t d_ = (d), x_ = (x); if (d_) { c->r[d_] = x_; if (d_ == ld_r) ld_r = 0; } } while (0)
#define LOAD(d, x) do { uint32_t d_ = (d), x_ = (x); if (d_) { c->ld_r = d_; c->ld_v = x_; } } while (0)
#define BRANCH(cond) do { if (cond) c->next_pc = c->pc + (SIMM << 2); c->branch = 1; } while (0)
    switch (op >> 26) {
    case 0x00:
        switch (op & 0x3f) {
        case 0x00: W(RD, t << SA); break;
        case 0x02: W(RD, t >> SA); break;
        case 0x03: W(RD, (uint32_t)((int32_t)t >> SA)); break;
        case 0x04: W(RD, t << (s & 31)); break;
        case 0x06: W(RD, t >> (s & 31)); break;
        case 0x07: W(RD, (uint32_t)((int32_t)t >> (s & 31))); break;
        case 0x08: c->next_pc = s; c->branch = 1; break;
        case 0x09: W(RD, c->pc + 4); c->next_pc = s; c->branch = 1; break;
        case 0x0c: cpu_exception(m, EXC_SYS); break;
        case 0x0d: cpu_exception(m, EXC_BP); break;
        case 0x10: W(RD, c->hi); break;
        case 0x11: c->hi = s; break;
        case 0x12: W(RD, c->lo); break;
        case 0x13: c->lo = s; break;
        case 0x18: { int64_t p = (int64_t)(int32_t)s * (int32_t)t; c->lo = (uint32_t)p; c->hi = (uint32_t)((uint64_t)p >> 32); break; }
        case 0x19: { uint64_t p = (uint64_t)s * t; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); break; }
        case 0x1a:   // division by zero and the one overflowing case give fixed results (psx-spx, CPU Arithmetic Opcodes)
            if (!t) { c->hi = s; c->lo = (int32_t)s < 0 ? 1 : 0xffffffff; }
            else if (s == 0x80000000 && t == 0xffffffff) { c->hi = 0; c->lo = 0x80000000; }
            else { c->lo = (uint32_t)((int32_t)s / (int32_t)t); c->hi = (uint32_t)((int32_t)s % (int32_t)t); }
            break;
        case 0x1b: if (!t) { c->hi = s; c->lo = 0xffffffff; } else { c->lo = s / t; c->hi = s % t; } break;
        case 0x20: { int32_t r; if (__builtin_add_overflow((int32_t)s, (int32_t)t, &r)) cpu_exception(m, EXC_OV); else W(RD, (uint32_t)r); break; }
        case 0x21: W(RD, s + t); break;
        case 0x22: { int32_t r; if (__builtin_sub_overflow((int32_t)s, (int32_t)t, &r)) cpu_exception(m, EXC_OV); else W(RD, (uint32_t)r); break; }
        case 0x23: W(RD, s - t); break;
        case 0x24: W(RD, s & t); break;
        case 0x25: W(RD, s | t); break;
        case 0x26: W(RD, s ^ t); break;
        case 0x27: W(RD, ~(s | t)); break;
        case 0x2a: W(RD, (int32_t)s < (int32_t)t); break;
        case 0x2b: W(RD, s < t); break;
        default: cpu_exception(m, EXC_RI); break;
        }
        break;
    case 0x01:   // BcondZ: bit 0 of rt picks GEZ or LTZ; rt 10h/11h (and their mirrors) link, taken or not (psx-spx, CPU Jump Opcodes)
        if ((RT & 0x1e) == 0x10) W(31, c->pc + 4);
        BRANCH(RT & 1 ? (int32_t)s >= 0 : (int32_t)s < 0);
        break;
    case 0x02: c->next_pc = (c->pc & 0xf0000000) | (op & 0x3ffffff) << 2; c->branch = 1; break;
    case 0x03: W(31, c->pc + 4); c->next_pc = (c->pc & 0xf0000000) | (op & 0x3ffffff) << 2; c->branch = 1; break;
    case 0x04: BRANCH(s == t); break;
    case 0x05: BRANCH(s != t); break;
    case 0x06: BRANCH((int32_t)s <= 0); break;
    case 0x07: BRANCH((int32_t)s > 0); break;
    case 0x08: { int32_t r; if (__builtin_add_overflow((int32_t)s, (int32_t)SIMM, &r)) cpu_exception(m, EXC_OV); else W(RT, (uint32_t)r); break; }
    case 0x09: W(RT, a); break;
    case 0x0a: W(RT, (int32_t)s < (int32_t)SIMM); break;
    case 0x0b: W(RT, s < SIMM); break;
    case 0x0c: W(RT, s & ZIMM); break;
    case 0x0d: W(RT, s | ZIMM); break;
    case 0x0e: W(RT, s ^ ZIMM); break;
    case 0x0f: W(RT, ZIMM << 16); break;
    case 0x10:
        if (RS == 0) LOAD(RT, c0[RD]);
        else if (RS == 4) cop0_write(m, RD, t);
        else if (op >> 25 & 1 && (op & 0x3f) == 0x10) c0[C0_SR] = (c0[C0_SR] & ~0xfu) | (c0[C0_SR] >> 2 & 0xf);   // RFE
        else { RED_ONCE(m, RED_COP0, "cpu: COP0 instruction %08x at %08x does not exist on the R3000A", op, c->cur_pc); cpu_exception(m, EXC_RI); }
        break;
    case 0x12:
        if (!(c0[C0_SR] & 1u << 30)) { unusable(m, 2); break; }
        if (op >> 25 & 1) { gte_command(m, op); break; }
        switch (RS) {
        case 0: LOAD(RT, gte_read(m, RD, 0)); break;
        case 2: LOAD(RT, gte_read(m, RD, 1)); break;
        case 4: gte_write(m, RD, t, 0); break;
        case 6: gte_write(m, RD, t, 1); break;
        case 8: if (RT <= 1) { BRANCH(!(RT & 1)); } else cpu_exception(m, EXC_RI); break;   // psx-spx, GTE Opcodes: COP2 condition input is tied low
        default: cpu_exception(m, EXC_RI); break;
        }
        break;
    case 0x11: case 0x13: unusable(m, op >> 26 & 3); break;
    case 0x20: LOAD(RT, (uint32_t)(int8_t)mem_read8(m, a)); break;
    case 0x21: if (a & 1) addr_error(m, a, EXC_ADEL); else LOAD(RT, (uint32_t)(int16_t)mem_read16(m, a)); break;
    case 0x23: if (a & 3) addr_error(m, a, EXC_ADEL); else LOAD(RT, mem_read32(m, a)); break;
    case 0x24: LOAD(RT, mem_read8(m, a)); break;
    case 0x25: if (a & 1) addr_error(m, a, EXC_ADEL); else LOAD(RT, mem_read16(m, a)); break;
    case 0x22: case 0x26: {   // LWL / LWR: the bytes of an unaligned word on each side of a word boundary, merged into rt
        uint32_t w = mem_read32(m, a & ~3u), cur = ld_r == RT ? ld_v : t, sh = (a & 3) * 8;
        if (op >> 26 == 0x22) LOAD(RT, (cur & (0x00ffffffu >> sh)) | w << (24 - sh));
        else LOAD(RT, (cur & ~(0xffffffffu >> sh)) | w >> sh);
        break;
    }
    case 0x28: if (!isolated) mem_write8(m, a, (uint8_t)t); break;
    case 0x29: if (a & 1) addr_error(m, a, EXC_ADES); else if (!isolated) mem_write16(m, a, (uint16_t)t); break;
    case 0x2b: if (a & 3) addr_error(m, a, EXC_ADES); else if (!isolated) mem_write32(m, a, t); break;
    case 0x2a: case 0x2e: {   // SWL / SWR
        if (isolated) break;
        uint32_t w = mem_read32(m, a & ~3u), sh = (a & 3) * 8;
        if (op >> 26 == 0x2a) w = (w & ~(0xffffffffu >> (24 - sh))) | t >> (24 - sh);
        else w = (w & ~(0xffffffffu << sh)) | t << sh;
        mem_write32(m, a & ~3u, w);
        break;
    }
    case 0x32:
        if (!(c0[C0_SR] & 1u << 30)) unusable(m, 2);
        else if (a & 3) addr_error(m, a, EXC_ADEL);
        else gte_write(m, RT, mem_read32(m, a), 0);
        break;
    case 0x3a:
        if (!(c0[C0_SR] & 1u << 30)) unusable(m, 2);
        else if (a & 3) addr_error(m, a, EXC_ADES);
        else if (!isolated) mem_write32(m, a, gte_read(m, RT, 0));
        break;
    case 0x30: case 0x31: case 0x33: case 0x38: case 0x39: case 0x3b: unusable(m, op >> 26 & 3); break;
    default: cpu_exception(m, EXC_RI); break;
    }
#undef W
#undef LOAD
#undef BRANCH
    if (ld_r) c->r[ld_r] = ld_v;
}

void cpu_run(ps1 *m) { while (m->cycles < m->next_event && !m->exit_requested) cpu_step(m); }
