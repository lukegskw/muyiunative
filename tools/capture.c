// The capture (plan.md, section 3: tools/ capture): what a program runs, for the code generator, from the interpreter at build time. OpenBIOS copies its kernel and
// its shell from ROM into RAM and enters them through jumps a static analysis cannot follow; a game loads code from its disc. So the machine runs the program in the
// interpreter for some frames, and the capture keeps RAM as it is then, and every address the CPU went to by a call (JAL, JALR, BLTZAL/BGEZAL taken) or by a jump
// through a register other than RA (a return) or K0/K1 (an exception's return): the starts of functions, which tools/codegen.py analyzes from (--capture). Native
// code is checked against memory where it runs, so code that changes after the capture runs in the interpreter, never wrongly.
// Usage: capture <bios.bin> <frames> <out> [program.exe]: writes <out>.ram (main RAM, 2 MB) and <out>.txt (the addresses, in hex, one a line)
#include "ps1.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t *a; size_t n, cap; } list;
static void add(list *l, uint32_t a) {
    if (l->n == l->cap) { l->cap = l->cap ? 2 * l->cap : 1024; l->a = realloc(l->a, l->cap * sizeof *l->a); if (!l->a) { fputs("capture: no memory\n", stderr); exit(1); } }
    l->a[l->n++] = a;
}
static int cmp(const void *x, const void *y) { uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y; return (a > b) - (a < b); }

static void step(ps1 *m, uint32_t pc, uint32_t op) {   // called after each instruction: next_pc is where a branch or jump goes after its delay slot
    uint32_t p = op >> 26, f = op & 0x3f, rs = op >> 21 & 31, to = m->cpu.next_pc;
    int link = p == 1 && (op >> 16 & 0x1e) == 0x10 && to != pc + 8;   // BLTZAL/BGEZAL, taken
    if (p == 3 || link || (p == 0 && f == 9) || (p == 0 && f == 8 && rs != 31 && rs != 26 && rs != 27)) add(m->step_ctx, to);
}
static void quiet(void *ctx, const char *line) { (void)ctx; (void)line; }

int main(int argc, char **argv) {
    if (argc < 4) { fputs("usage: capture <bios.bin> <frames> <out> [program.exe]\n", stderr); return 2; }
    ps1 *m = ps1_new();
    if (!m || ps1_load_bios(m, argv[1])) return 1;
    if (argc > 4) {
        FILE *f = fopen(argv[4], "rb"); static uint8_t buf[RAM_SIZE]; size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
        if (f) fclose(f);
        if (!n || ps1_load_exe(m, buf, n)) { fprintf(stderr, "capture: cannot load %s\n", argv[4]); return 1; }
    }
    list l = {0};
    m->on_step = step; m->step_ctx = &l; m->tty_out = quiet;
    int frames = atoi(argv[2]), i = 0;
    for (; i < frames && !m->exit_requested; i++) ps1_run_frame(m);
    qsort(l.a, l.n, sizeof *l.a, cmp);
    char path[1024]; size_t k = 0;
    snprintf(path, sizeof path, "%s.txt", argv[3]);
    FILE *t = fopen(path, "w");
    for (size_t j = 0; t && j < l.n; j++) if (!j || l.a[j] != l.a[j - 1]) { fprintf(t, "%08x\n", l.a[j]); k++; }
    snprintf(path, sizeof path, "%s.ram", argv[3]);
    FILE *r = fopen(path, "wb");
    int ok = t && r && fwrite(m->ram, 1, RAM_SIZE, r) == RAM_SIZE;
    if (t && fclose(t)) ok = 0;
    if (r && fclose(r)) ok = 0;
    if (!ok) { fprintf(stderr, "capture: cannot write %s\n", argv[3]); return 1; }
    printf("capture: %d frames, %zu addresses called or jumped to, RAM into %s.ram\n", i, k, argv[3]);
    free(l.a); ps1_free(m);
    return 0;
}
