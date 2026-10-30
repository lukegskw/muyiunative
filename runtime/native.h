// Native code for guest code (plan.md, section 2: the lookup table; phase 3): what tools/codegen.py generates and the runtime runs. A function of native code is
// entered at the start of any of its blocks (m->cpu.pc), runs the instructions of cpu_ops.h with their words and addresses as constants, goes from block to block
// itself, and returns to the dispatcher (cpu_run) for a call, a return, a jump it cannot follow, an exception, or, at the end of a block, a device event or an
// interrupt that is due. The dispatcher enters it only while memory holds the guest code it was generated from (native.c), so code loaded over it later (an overlay,
// another program) runs in the interpreter
#pragma once
#include "cpu_ops.h"

typedef struct ps1_native_fn {
    void (*run)(ps1 *m); uint32_t start;   // start: the guest function's address
    const uint32_t (*runs)[2]; unsigned nruns; const uint32_t *words;   // the guest code it was made from: runs of addresses [start, end), and their words in order
} ps1_native_fn;
typedef struct ps1_native_entry { uint32_t pc, fn; } ps1_native_entry;   // a block's start and the function it is in, sorted by pc
struct ps1_native_set { const char *name; const ps1_native_fn *fns; unsigned nfns; const ps1_native_entry *entries; unsigned nentries; };

// One instruction of native code: its fetch's cycles, the operation, the load in flight landing; back to the dispatcher after an exception
#define NATIVE_OP(name, pc, op) do { \
    cpu_ld l_ = cpu_begin(m, pc, mem_fetch_cycles(pc)); int x_ = op_##name(m, &l_, op, mem_fetch_cycles(pc)); cpu_end(m, l_); if (x_) return; } while (0)
// At the end of a block: go on to the next block, unless the dispatcher has a device event, an interrupt or an exit to see to
#define NATIVE_GO (m->cycles < m->next_event && !m->exit_requested && !cpu_irq_due(m))
