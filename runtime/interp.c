// The R3000A interpreter (psx-spx, CPU Specifications; the MIPS I instruction set): one instruction per step, with the definitions of cpu_ops.h, which the native code
// the tools generate shares (plan.md, phase 3). A loaded register reaches the CPU one instruction late (the load delay slot), LWL/LWR merge with a load in flight, and
// the instruction after a branch or jump runs before the branch takes effect (the branch delay slot). COP2 reaches the GTE. An instruction takes the cycles of its fetch,
// plus a load's wait (mem.c); MULT and DIV run on beside the CPU, and MFHI/MFLO wait for them. This interpreter is the reference native code is checked against
// (AGENTS.md); cpu_run is the dispatcher between the two
#include "cpu_ops.h"

void cpu_step(ps1 *m) {   // one instruction, or the exception its fetch raises: an address error for a misaligned PC, a bus error where nothing executable is mapped
    ps1_cpu *c = &m->cpu;
    uint32_t op;
    if (!(c->pc & 3) && mem_fetch(m, c->pc, &op)) {
        uint32_t pc = c->pc;
        m->interp_steps++; cpu_exec(m, op, mem_fetch_cycles(pc));
        if (m->on_step) m->on_step(m, pc, op);
        return;
    }
    c->delay = c->branch; c->branch = 0; c->cur_pc = c->pc; m->cycles++;
    if (c->pc & 3) { c->cop0[C0_BADVADDR] = c->pc; cpu_exception(m, EXC_ADEL); }
    else cpu_exception(m, EXC_IBE);
}

// The dispatcher: native code where the machine has some for the code in memory (native.c), else the interpreter. Only at the end of a block (cpu.boundary) does it stop
// for the next device event, take an interrupt (plan.md, decision log: both sides then see each interrupt at the same point, so the lockstep comparison is exact), hear
// the kernel's std_out_putchar, A(3Ch) and B(3Dh) (psx-spx, BIOS Function Summary), for the debug console, and start a side-loaded EXE (exe.c). A delay slot is always
// interpreted: native code is entered at the start of a block
void cpu_run(ps1 *m) {
    ps1_cpu *c = &m->cpu;
    for (;;) {
        if (c->boundary) {
            if (m->cycles >= m->next_event || m->exit_requested) return;
            if (cpu_irq_due(m)) {
                c->delay = 0; c->cur_pc = c->pc; m->cycles++;
                // Crash's hub trace interrupted RTPT, then the BIOS skipped it on return while SXY still held the preceding triangle. psx-spx, CPU Specifications,
                // COP0 Exception Handling / Interrupts vs GTE Commands: a GTE command at the interrupted PC still issues, but EPC points to that same opcode.
                // Keep normal IRQ entry (including its pending load and EPC); transfers and disabled COP2 do not issue a command as this side effect.
                uint32_t op;
                if ((c->cop0[C0_SR] & (1u << 30)) && !(c->pc & 3) && mem_fetch(m, c->pc, &op) && (op & 0xfe000000u) == 0x4a000000u)
                    gte_command(m, op);
                cpu_exception(m, EXC_INT); continue;
            }
            uint32_t p = c->pc & 0x1fffffff;
            if ((p == 0xa0 && c->r[9] == 0x3c) || (p == 0xb0 && c->r[9] == 0x3d)) tty_putc(m, (char)c->r[4]);
            else if (m->exe && ps1_exe_due(m)) ps1_exe_start(m);
        }
        if (c->branch || !native_run(m)) cpu_step(m);
    }
}
