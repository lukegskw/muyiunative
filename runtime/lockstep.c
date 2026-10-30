// The lockstep comparison (plan.md, section 6, Testing: native code is right when it matches the interpreter in lockstep; phase 3). Two machines run from the same
// state, ref interpreting everything and nat running native code where it has some, and stop together: next_event is `step` cycles ahead, so each cpu_run returns
// at the end of the first block past it, where both take the same device events and interrupts (plan.md, decision log: interrupts at the end of a block). A step of
// 1 stops at every block; a longer one lets native code go on from block to block by itself, as it does outside the comparison.
// The CPU and the cycle count are compared at every stop; memory and the devices every full_every stops and at the end of each frame. The first difference is
// reported by name (a register, a COP0 or GTE register, a RAM address or a VRAM pixel, a device), with the native block that ran last on nat, or "interpreted".
// Not compared: what differs by design (the native code attached, the counters of what ran natively), and the host's side (the disc's files, the tty sink)
#include "native.h"
#include <stddef.h>
#include <string.h>

static const char *const reg_names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                                          "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

static int cpu_differ(const ps1 *a, const ps1 *b, char *out, size_t n) {   // where the CPU is first: a different path tells more than the registers it left
    const ps1_cpu *x = &a->cpu, *y = &b->cpu;
#define F(f) if (x->f != y->f) return snprintf(out, n, #f " %08llx, native %08llx", (unsigned long long)x->f, (unsigned long long)y->f), 1
    F(pc); F(next_pc);
    for (int i = 0; i < 32; i++) if (x->r[i] != y->r[i]) return snprintf(out, n, "%s %08x, native %08x", reg_names[i], x->r[i], y->r[i]), 1;
    F(cur_pc); F(hi); F(lo); F(ld_r); F(ld_v); F(branch); F(delay); F(boundary); F(gte_busy); F(hilo_busy);
#undef F
    for (int i = 0; i < 32; i++) if (x->cop0[i] != y->cop0[i]) return snprintf(out, n, "cop0 r%d %08x, native %08x", i, x->cop0[i], y->cop0[i]), 1;
    for (int i = 0; i < 32; i++) if (x->cop2d[i] != y->cop2d[i]) return snprintf(out, n, "GTE data r%d %08x, native %08x", i, x->cop2d[i], y->cop2d[i]), 1;
    for (int i = 0; i < 32; i++) if (x->cop2c[i] != y->cop2c[i]) return snprintf(out, n, "GTE control r%d %08x, native %08x", i, x->cop2c[i], y->cop2c[i]), 1;
    if (a->cycles != b->cycles) return snprintf(out, n, "cycles %llu, native %llu", (unsigned long long)a->cycles, (unsigned long long)b->cycles), 1;
    return 0;
}

#define PART(f) {#f, offsetof(ps1, f), sizeof ((ps1 *)0)->f}
static const struct { const char *name; size_t at, n; } parts[] = {
    PART(ram), PART(scratch), PART(bios), PART(next_event), PART(i_stat), PART(i_mask), PART(memctl), PART(ram_size), PART(cache_ctl), PART(dpcr), PART(dicr),
    PART(post), PART(dma), PART(tmr), PART(gpu), PART(cd), PART(spu), PART(sio), PART(mdec), PART(tty), PART(ntty), PART(exe_n), PART(frame_done),
    PART(exit_requested), PART(exit_code), PART(red), PART(unmapped)};
#undef PART

static int full_differ(const ps1 *a, const ps1 *b, char *out, size_t n) {
    if (cpu_differ(a, b, out, n)) return 1;
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++) {
        const uint8_t *x = (const uint8_t *)a + parts[i].at, *y = (const uint8_t *)b + parts[i].at;
        if (!memcmp(x, y, parts[i].n)) continue;
        size_t k = 0;
        while (x[k] == y[k]) k++;
        if (parts[i].at == offsetof(ps1, ram)) { k &= ~(size_t)3; uint32_t v, w; memcpy(&v, x + k, 4); memcpy(&w, y + k, 4); return snprintf(out, n, "RAM at %06zx: %08x, native %08x", k, v, w), 1; }
        if (parts[i].at == offsetof(ps1, gpu) && k < sizeof a->gpu.vram)
            return snprintf(out, n, "VRAM at (%zu, %zu): %04x, native %04x", k / 2 % VRAM_W, k / 2 / VRAM_W, a->gpu.vram[k / 2], b->gpu.vram[k / 2]), 1;
        return snprintf(out, n, "%s, at byte %zu of %zu", parts[i].name, k, parts[i].n), 1;
    }
    return 0;
}

static int report(const ps1 *a, const ps1 *b, ps1_lockstep *ls, int native, const char *what) {
    char where[160];
    if (native) snprintf(where, sizeof where, "after the native block at %08x (function %08x)", b->native_pc, b->native_fn);
    else snprintf(where, sizeof where, "after interpreted code");
    snprintf(ls->report, sizeof ls->report, "stop %llu, cycle %llu, pc %08x (native %08x), %s: %s", (unsigned long long)ls->stops,
             (unsigned long long)a->cycles, a->cpu.cur_pc, b->cpu.cur_pc, where, what);
    return -1;
}

int ps1_lockstep_frame(ps1 *a, ps1 *b, ps1_lockstep *ls) {
    char what[200];
    a->frame_done = b->frame_done = 0;
    while (!a->frame_done && !a->exit_requested) {
        uint64_t stop = a->cycles + (ls->step ? ls->step : 1), runs = b->native_runs;
        if (a->next_event > stop) a->next_event = stop;
        if (b->next_event > stop) b->next_event = stop;
        cpu_run(a); cpu_run(b); sched_run(a); sched_run(b);
        ls->stops++;
        if (cpu_differ(a, b, what, sizeof what) || (ls->full_every && ls->stops % ls->full_every == 0 && full_differ(a, b, what, sizeof what)))
            return report(a, b, ls, b->native_runs != runs, what);
    }
    return full_differ(a, b, what, sizeof what) ? report(a, b, ls, 0, what) : 0;
}
