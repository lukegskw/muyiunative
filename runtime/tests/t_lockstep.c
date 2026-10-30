// The lockstep comparison (lockstep.c), plan.md's check that native code matches the interpreter. First on itself: native code written here with a wrong word, as a
// generator bug would leave it, must be caught at the first stop, the report naming the register or the RAM address, the values and the native block. Then OpenBIOS
// (plan.md, phase 3: recompiled OpenBIOS), whose native code the build generates from the bundled binary and a capture of what it runs (tools/capture.c): from
// reset to its shell on two machines, one interpreting and one with the native code, which must agree throughout and leave almost nothing to the interpreter.
// Then four ps1-tests programs (tests/ps1-tests), whose native code the build generates too (tools/codegen.py, into the build folder), under OpenBIOS from reset
// to past the program's start; two stop at every block, two every 64 cycles so that native code goes from block to block by itself
#include "native.h"
#include <stdlib.h>
#include <string.h>

#ifndef BIOS_PATH
#error "BIOS_PATH is the bundled OpenBIOS (runtime/tests/Makefile passes it)"
#endif
#ifndef TESTS_DIR
#error "TESTS_DIR is the repository's tests folder (runtime/tests/Makefile passes it)"
#endif

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fputs("FAIL: ", stderr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// The guest: addiu v0, zero, 1; sw v0, 100h(zero); beq zero, zero, -12 (back to the start); nop
static const uint32_t guest[] = {0x24020001, 0xac020100, 0x1000fffd, 0x00000000};
static const uint32_t guest_runs[][2] = {{0x80010000u, 0x80010010u}};
static void wrong_add(ps1 *m) {   // addiu v0, zero, 2
    switch (m->cpu.pc) { case 0x80010000u: goto b; default: return; }
b:
    NATIVE_OP(addiu, 0x80010000u, 0x24020002u); NATIVE_OP(sw, 0x80010004u, 0xac020100u); NATIVE_OP(beq, 0x80010008u, 0x1000fffdu); NATIVE_OP(sll, 0x8001000cu, 0u);
}
static void wrong_store(ps1 *m) {   // sw v0, 104h(zero)
    switch (m->cpu.pc) { case 0x80010000u: goto b; default: return; }
b:
    NATIVE_OP(addiu, 0x80010000u, 0x24020001u); NATIVE_OP(sw, 0x80010004u, 0xac020104u); NATIVE_OP(beq, 0x80010008u, 0x1000fffdu); NATIVE_OP(sll, 0x8001000cu, 0u);
}
static const ps1_native_fn wrong_fns[] = {{wrong_add, 0x80010000u, guest_runs, 1, guest}, {wrong_store, 0x80010000u, guest_runs, 1, guest}};
static const ps1_native_entry entry0[] = {{0x80010000u, 0}}, entry1[] = {{0x80010000u, 1}};
static const ps1_native_set wrong_add_set = {"wrong_add", wrong_fns, 2, entry0, 1}, wrong_store_set = {"wrong_store", wrong_fns, 2, entry1, 1};

static ps1 *bare(const ps1_native_set *s) {   // a machine with the guest at 80010000h and no BIOS
    ps1 *m = ps1_new();
    if (!m) { fputs("FAIL: no memory for the machine\n", stderr); exit(1); }
    memcpy(m->ram + 0x10000, guest, sizeof guest);
    m->cpu.cop0[C0_SR] = 0; m->cpu.pc = 0x80010000u; m->cpu.next_pc = m->cpu.pc + 4;
    if (s) ps1_native_add(m, s);
    return m;
}

static void quiet(void *ctx, const char *line) { (void)ctx; (void)line; }
static ps1 *booted(const char *exe, const ps1_native_set *s) {   // OpenBIOS from reset, which starts the program, if any, in place of its shell
    ps1 *m = ps1_new();
    if (!m || ps1_load_bios(m, BIOS_PATH)) { fputs("FAIL: cannot load the BIOS\n", stderr); exit(1); }
    if (exe) {
        char path[512]; snprintf(path, sizeof path, "%s/ps1-tests/%s", TESTS_DIR, exe);
        static uint8_t buf[1 << 20]; FILE *f = fopen(path, "rb"); size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
        if (f) fclose(f);
        if (ps1_load_exe(m, buf, n)) { fprintf(stderr, "FAIL: cannot start %s\n", exe); exit(1); }
    }
    m->tty_out = quiet;
    if (s) { extern const ps1_native_set native_openbios; ps1_native_add(m, &native_openbios); if (s != &native_openbios) ps1_native_add(m, s); }
    return m;
}

extern const ps1_native_set native_openbios, native_cop, native_test_all, native_otc_test, native_triangle;

static int in_step(ps1 *a, ps1 *b, ps1_lockstep *ls, int frames) {   // frames run while the machines agree
    int f = 0;
    while (f < frames && !a->exit_requested && !ps1_lockstep_frame(a, b, ls)) f++;
    return f;
}

int main(void) {
    ps1 *a = bare(NULL), *b = bare(&wrong_add_set);
    ps1_lockstep ls = {0};
    CHECK(ps1_lockstep_frame(a, b, &ls) && ls.stops == 1 && strstr(ls.report, "after the native block at 80010000 (function 80010000): v0 00000001, native 00000002"),
          "a wrong register is reported at the first stop: %s", ls.report);
    ps1_free(a); ps1_free(b);
    a = bare(NULL); b = bare(&wrong_store_set); ls = (ps1_lockstep){.full_every = 1};
    CHECK(ps1_lockstep_frame(a, b, &ls) && ls.stops == 1 && strstr(ls.report, "RAM at 000100: 00000001, native 00000000"),
          "a wrong store is reported at the first stop when memory is compared at each: %s", ls.report);
    ps1_free(a); ps1_free(b);

    // OpenBIOS reaches its shell at frame 37 (t_ps1tests.c); 50 frames take it into the shell's loop
    a = booted(NULL, NULL); b = booted(NULL, &native_openbios);
    ls = (ps1_lockstep){.step = 1, .full_every = 10000};
    int f = in_step(a, b, &ls, 50);
    CHECK(f == 50, "OpenBIOS, frame %d: %s", f, ls.report);
    CHECK(b->interp_steps * 200 < a->interp_steps, "OpenBIOS: %llu of %llu instructions interpreted", (unsigned long long)b->interp_steps,
          (unsigned long long)a->interp_steps);
    fprintf(stderr, "lockstep: OpenBIOS, %d frames, %llu stops in step, native code entered %llu times, %llu of %llu instructions interpreted\n", f,
            (unsigned long long)ls.stops, (unsigned long long)b->native_runs, (unsigned long long)b->interp_steps, (unsigned long long)a->interp_steps);
    ps1_free(a); ps1_free(b);

    static const struct { const char *exe; const ps1_native_set *set; unsigned step; int frames; } programs[] = {
        {"cpu/cop/cop.exe", &native_cop, 1, 45}, {"gte/test-all/test-all.exe", &native_test_all, 64, 60},
        {"dma/otc-test/otc-test.exe", &native_otc_test, 1, 45}, {"gpu/triangle/triangle.exe", &native_triangle, 64, 60}};
    for (size_t i = 0; i < sizeof programs / sizeof *programs; i++) {
        a = booted(programs[i].exe, NULL); b = booted(programs[i].exe, programs[i].set);
        ls = (ps1_lockstep){.step = programs[i].step, .full_every = 10000};
        f = in_step(a, b, &ls, programs[i].frames);
        CHECK(f == programs[i].frames || a->exit_requested, "%s, frame %d: %s", programs[i].exe, f, ls.report);
        CHECK(b->native_runs > 1000, "%s: native code entered %llu times", programs[i].exe, (unsigned long long)b->native_runs);
        fprintf(stderr, "lockstep: %s, %d frames, %llu stops in step, native code entered %llu times, %llu of %llu instructions interpreted\n", programs[i].exe, f,
                (unsigned long long)ls.stops, (unsigned long long)b->native_runs, (unsigned long long)b->interp_steps, (unsigned long long)a->interp_steps);
        ps1_free(a); ps1_free(b);
    }
    fprintf(stderr, "lockstep: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
