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

static void step(ps1 *m, uint32_t pc, uint32_t op) {   // after each interpreted instruction: next_pc is where a branch or jump goes after its delay slot
    uint32_t p = op >> 26, f = op & 0x3f, rs = op >> 21 & 31, to = m->cpu.next_pc;
    int link = p == 1 && (op >> 16 & 0x1e) == 0x10 && to != pc + 8;   // BLTZAL/BGEZAL, taken
    if (p == 3 || link || (p == 0 && f == 9) || (p == 0 && f == 8 && rs != 31 && rs != 26 && rs != 27)) add(m->step_ctx, to);
}

void ps1_capture_start(ps1 *m, ps1_capture *c) { *c = (ps1_capture){0}; m->on_step = step; m->step_ctx = c; }

static int cmp(const void *x, const void *y) { uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y; return (a > b) - (a < b); }

int ps1_capture_write(const ps1 *m, ps1_capture *c, const char *out) {
    if (c->failed) { RED("capture: no memory for the addresses"); return -1; }
    qsort(c->a, c->n, sizeof *c->a, cmp);
    char path[1024]; size_t k = 0;
    snprintf(path, sizeof path, "%s.txt", out);
    FILE *t = fopen(path, "w");
    for (size_t j = 0; t && j < c->n; j++) if (!j || c->a[j] != c->a[j - 1]) { fprintf(t, "%08x\n", c->a[j]); k++; }
    snprintf(path, sizeof path, "%s.ram", out);
    FILE *r = fopen(path, "wb");
    int ok = t && r && fwrite(m->ram, 1, RAM_SIZE, r) == RAM_SIZE;
    if (t && fclose(t)) ok = 0;
    if (r && fclose(r)) ok = 0;
    if (!ok) { RED("capture: cannot write %s.txt and %s.ram", out, out); return -1; }
    LOG("capture: %zu addresses called or jumped to, and RAM, into %s.txt and %s.ram", k, out, out);
    free(c->a); *c = (ps1_capture){0};
    return 0;
}
