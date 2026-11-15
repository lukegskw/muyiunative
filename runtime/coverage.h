// Native execution evidence, owned by a headless host rather than saved console state. Only addresses, reasons and counters are exported: never guest bytes.
// The audit found a passing lockstep route could still interpret most of its program. Start at the executable entry and report address regions separately.
#pragma once
#include "ps1.h"

enum { COVER_PROGRAM, COVER_KERNEL, COVER_BIOS, COVER_OTHER, COVER_REGIONS };
enum { FALLBACK_NO_ENTRY, FALLBACK_CHANGED, FALLBACK_DELAY, FALLBACK_DISABLED, FALLBACK_REASONS };
typedef struct ps1_fallback ps1_fallback;
struct ps1_coverage {
    uint64_t native[COVER_REGIONS], interpreted[COVER_REGIONS];
    uint64_t operations[2];   // operation categories exercised by generated code; not static game-code coverage
    uint32_t entry; int started, failed;
    ps1_fallback *buckets[4096];
};

static inline unsigned coverage_region(uint32_t pc) {
    uint32_t p = pc >> 29 == 4 || pc >> 29 == 5 ? pc & 0x1fffffff : pc;
    if (p < 0x800000) return (p & (RAM_SIZE - 1)) < 0x10000 ? COVER_KERNEL : COVER_PROGRAM;
    return p >= 0x1fc00000 && p < 0x1fc00000 + BIOS_SIZE ? COVER_BIOS : COVER_OTHER;
}
static inline int coverage_due(ps1_coverage *c, uint32_t pc) {
    if (!c->started && (!c->entry || c->entry == pc)) c->started = 1;
    return c->started;
}
static inline void coverage_native(ps1 *m, uint32_t pc, unsigned op) {
    ps1_coverage *c = m->coverage;
    if (c && coverage_due(c, pc)) { c->native[coverage_region(pc)]++; c->operations[op / 64] |= UINT64_C(1) << (op % 64); }
}
void coverage_interpreted(ps1 *m, uint32_t pc);
int ps1_coverage_write(const ps1_coverage *c, const char *path);   // JSON; nonzero on I/O/allocation failure
void ps1_coverage_free(ps1_coverage *c);
