// The lookup table (plan.md, section 2): from the CPU's pc to the native code generated for the block that starts there, in the sets attached to the machine. Native
// code runs only while memory holds the guest code it was made from, compared word for word on each entry from the dispatcher; elsewhere the interpreter runs
#include "native.h"
#include <string.h>

int ps1_native_add(ps1 *m, const ps1_native_set *s) {
    if (m->nnative == (int)(sizeof m->native / sizeof *m->native)) { RED("native: no room for the native code of %s", s->name); return -1; }
    m->native[m->nnative++] = s;
    return 0;
}

static int matches(ps1 *m, const ps1_native_fn *f) {
    const uint32_t *w = f->words;
    for (unsigned i = 0; i < f->nruns; i++) {
        uint32_t a = f->runs[i][0], n = f->runs[i][1] - a;
        const uint8_t *p = mem_code(m, a, n);
        if (!p || memcmp(p, w, n)) return 0;
        w += n / 4;
    }
    return 1;
}

int native_run(ps1 *m) {
    uint32_t pc = m->cpu.pc;
    uint64_t at = m->cycles;
    for (int k = 0; k < m->nnative; k++) {
        const ps1_native_set *s = m->native[k];
        unsigned lo = 0, hi = s->nentries;
        while (lo < hi) { unsigned mid = lo + (hi - lo) / 2; if (s->entries[mid].pc < pc) lo = mid + 1; else hi = mid; }
        for (; lo < s->nentries && s->entries[lo].pc == pc; lo++) {   // run: at least one instruction runs, unless the set's table is wrong
            const ps1_native_fn *f = &s->fns[s->entries[lo].fn];
            if (matches(m, f)) { m->native_runs++; m->native_pc = pc; m->native_fn = f->start; f->run(m); return m->cycles != at; }
        }
    }
    return 0;
}
