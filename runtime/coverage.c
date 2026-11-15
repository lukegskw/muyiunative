#include "coverage.h"
#include <stdlib.h>

struct ps1_fallback { ps1_fallback *next; uint32_t pc; unsigned reason; uint64_t hits; };

void coverage_interpreted(ps1 *m, uint32_t pc) {
    ps1_coverage *c = m->coverage;
    if (!c || !coverage_due(c, pc)) return;
    c->interpreted[coverage_region(pc)]++;
    unsigned reason = m->nnative ? m->native_miss : FALLBACK_DISABLED;
    unsigned bucket = ((pc >> 2) ^ (reason * 16777619u)) % 4096;
    ps1_fallback *f = c->buckets[bucket];
    while (f && (f->pc != pc || f->reason != reason)) f = f->next;
    if (!f) {
        f = calloc(1, sizeof *f);
        if (!f) { c->failed = 1; return; }
        f->pc = pc; f->reason = reason; f->next = c->buckets[bucket]; c->buckets[bucket] = f;
    }
    f->hits++;
}

static int order(const void *a, const void *b) {
    const ps1_fallback *x = *(const ps1_fallback *const *)a, *y = *(const ps1_fallback *const *)b;
    if (x->hits != y->hits) return x->hits > y->hits ? -1 : 1;
    if (x->pc != y->pc) return x->pc < y->pc ? -1 : 1;
    return (x->reason > y->reason) - (x->reason < y->reason);
}

int ps1_coverage_write(const ps1_coverage *c, const char *path) {
    static const char *const regions[] = {"program_ram", "kernel_ram", "bios_rom", "other"};
    static const char *const reasons[] = {"no_entry", "code_changed", "delay_slot", "disabled"};
    if (!c || c->failed) return -1;
    size_t n = 0;
    for (unsigned i = 0; i < 4096; i++) for (ps1_fallback *f = c->buckets[i]; f; f = f->next) n++;
    const ps1_fallback **sorted = malloc((n ? n : 1) * sizeof *sorted);
    if (!sorted) return -1;
    size_t k = 0;
    for (unsigned i = 0; i < 4096; i++) for (ps1_fallback *f = c->buckets[i]; f; f = f->next) sorted[k++] = f;
    qsort(sorted, n, sizeof *sorted, order);
    FILE *out = fopen(path, "w");
    if (!out) { free(sorted); return -1; }
    fprintf(out, "{\n  \"version\": 1, \"entry\": \"%08x\", \"started\": %s,\n  \"regions\": {\n", c->entry, c->started ? "true" : "false");
    for (unsigned i = 0; i < COVER_REGIONS; i++)
        fprintf(out, "    \"%s\": {\"native\": %llu, \"interpreted\": %llu}%s\n", regions[i], (unsigned long long)c->native[i],
                (unsigned long long)c->interpreted[i], i + 1 == COVER_REGIONS ? "" : ",");
    fprintf(out, "  },\n  \"native_operation_mask\": [\"%016llx\", \"%016llx\"],\n  \"fallbacks\": [", (unsigned long long)c->operations[0], (unsigned long long)c->operations[1]);
    for (size_t i = 0; i < n; i++) {
        const ps1_fallback *f = sorted[i];
        fprintf(out, "%s\n    {\"pc\": \"%08x\", \"region\": \"%s\", \"reason\": \"%s\", \"hits\": %llu}", i ? "," : "", f->pc,
                regions[coverage_region(f->pc)], reasons[f->reason < FALLBACK_REASONS ? f->reason : FALLBACK_NO_ENTRY], (unsigned long long)f->hits);
    }
    fputs("\n  ]\n}\n", out);
    int bad = ferror(out); if (fclose(out)) bad = 1;
    free(sorted); return bad ? -1 : 0;
}

void ps1_coverage_free(ps1_coverage *c) {
    for (unsigned i = 0; i < 4096; i++) {
        ps1_fallback *f = c->buckets[i];
        while (f) { ps1_fallback *next = f->next; free(f); f = next; }
        c->buckets[i] = NULL;
    }
}
