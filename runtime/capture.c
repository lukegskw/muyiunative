// The capture (plan.md, section 3: tools/ capture): what a program runs, for the code generator. OpenBIOS copies its kernel and its shell from ROM into RAM and enters
// them through jumps a static analysis cannot follow; a game loads code from its disc. So the machine runs the program in the interpreter, and the capture keeps every
// address the CPU went to by a call (JAL, JALR, BLTZAL/BGEZAL taken) or by a jump through a register other than RA (a return) or K0/K1 (an exception's return): the
// starts of functions, which tools/codegen.py analyzes from (--capture), with RAM as it is at the end. Only interpreted instructions are seen, so native code already
// attached keeps its own out of the capture. Native code is checked against memory where it runs, so code that changes after the capture is interpreted, never run
// wrongly. The capture is the host's (tools/capture.c, the Mac host's --capture), not the machine's: its list is not machine state
#include "ps1.h"
#include <stdlib.h>
#include <string.h>

static void add(ps1_capture *c, uint32_t a) {
    if (c->n == c->cap) {
        uint32_t *g = realloc(c->a, (c->cap ? 2 * c->cap : 1024) * sizeof *c->a);
        if (!g) { c->failed = 1; return; }
        c->a = g; c->cap = c->cap ? 2 * c->cap : 1024;
    }
    c->a[c->n++] = a;
}

// Phase 4 preserves code before it is overwritten. Final RAM alone loses transient overlays (t_overlays): keep the actual instruction words of each executed block,
// deduplicated by address and bytes, with a hit count for the survey. Hash collisions compare all bytes. The host owns these records; neither machine state nor time
// depends on them. PS1BLK1 records are pc/u32, word count/u32, hit count/u64 and words/u32, all little endian; tools/overlays.py computes their SHA-256 content keys.
struct ps1_capture_block { ps1_capture_block *next; uint32_t pc, n; uint64_t hits; uint32_t words[]; };

static void flush(ps1_capture *c) {
    if (!c->nwords) return;
    uint32_t h = c->pc;
    for (unsigned i = 0; i < c->nwords; i++) h = (h ^ c->words[i]) * 16777619u;
    unsigned bucket = h % 4096;
    ps1_capture_block *b = c->blocks[bucket];
    for (; b; b = b->next) if (b->pc == c->pc && b->n == c->nwords && !memcmp(b->words, c->words, 4 * b->n)) break;
    if (b) b->hits++;
    else {
        b = malloc(sizeof *b + 4 * c->nwords);
        if (!b) c->failed = 1;
        else {
            b->pc = c->pc; b->n = c->nwords; b->hits = 1; memcpy(b->words, c->words, 4 * b->n);
            b->next = c->blocks[bucket]; c->blocks[bucket] = b;
        }
    }
    c->nwords = 0;
}

static void step(ps1 *m, uint32_t pc, uint32_t op) {   // after each interpreted instruction: next_pc is where a branch or jump goes after its delay slot
    ps1_capture *c = m->step_ctx;
    if (c->failed) return;
    if (c->nwords && pc != c->pc + 4 * c->nwords) flush(c);
    if (!c->nwords) c->pc = pc;
    c->words[c->nwords++] = op;
    if (m->cpu.boundary || (c->nwords >= 256 && !m->cpu.branch)) flush(c);
    uint32_t p = op >> 26, f = op & 0x3f, rs = op >> 21 & 31, to = m->cpu.next_pc;
    int link = p == 1 && (op >> 16 & 0x1e) == 0x10 && to != pc + 8;   // BLTZAL/BGEZAL, taken
    if (p == 3 || link || (p == 0 && f == 9) || (p == 0 && f == 8 && rs != 31 && rs != 26 && rs != 27)) add(m->step_ctx, to);
}

void ps1_capture_start(ps1 *m, ps1_capture *c) { *c = (ps1_capture){0}; m->on_step = step; m->step_ctx = c; }

static int cmp(const void *x, const void *y) { uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y; return (a > b) - (a < b); }

int ps1_capture_write(ps1 *m, ps1_capture *c, const char *out) {
    if (m->step_ctx == c) { m->on_step = NULL; m->step_ctx = NULL; }
    flush(c);
    int ok = !c->failed;
    if (c->n) qsort(c->a, c->n, sizeof *c->a, cmp);
    char path[1024]; size_t k = 0, nb = 0;
    snprintf(path, sizeof path, "%s.txt", out);
    FILE *t = ok ? fopen(path, "w") : NULL;
    if (!t) ok = 0;
    for (size_t j = 0; t && j < c->n; j++) if (!j || c->a[j] != c->a[j - 1]) { if (fprintf(t, "%08x\n", c->a[j]) < 0) ok = 0; k++; }
    if (t && fclose(t)) ok = 0;
    snprintf(path, sizeof path, "%s.ram", out);
    FILE *r = ok ? fopen(path, "wb") : NULL;
    if (!r || fwrite(m->ram, 1, RAM_SIZE, r) != RAM_SIZE) ok = 0;
    if (r && fclose(r)) ok = 0;
    snprintf(path, sizeof path, "%s.blocks", out);
    FILE *blocks = ok ? fopen(path, "wb") : NULL;
    if (!blocks || fwrite("PS1BLK1", 1, 8, blocks) != 8) ok = 0;
    for (unsigned i = 0; i < 4096; i++) for (ps1_capture_block *b = c->blocks[i], *next; b; b = next) {
        next = b->next; nb++;
        if (ok) ok = fwrite(&b->pc, 4, 1, blocks) == 1 && fwrite(&b->n, 4, 1, blocks) == 1 && fwrite(&b->hits, 8, 1, blocks) == 1 && fwrite(b->words, 4, b->n, blocks) == b->n;
        free(b);
    }
    if (blocks && fclose(blocks)) ok = 0;
    free(c->a); *c = (ps1_capture){0};
    if (!ok) { RED("capture: cannot write %s.txt, .ram and .blocks (I/O or allocation failure)", out); return -1; }
    LOG("capture: %zu addresses called or jumped to, RAM and %zu block versions into %s.txt, .ram and .blocks", k, nb, out);
    return 0;
}
