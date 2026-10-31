// The capture at build time (plan.md, section 3: tools/ capture; runtime/capture.c): OpenBIOS, or a program it starts, run in the interpreter for some frames, for
// what tools/codegen.py --capture analyzes, including immutable block versions for overlays (phase 4). The Mac host's --capture does the same for a game, along its route.
// Usage: capture <bios.bin> <frames> <out> [program.exe]: writes <out>.ram (main RAM, 2 MB), .txt (call targets, hex) and .blocks (the code versions that ran)
#include "ps1.h"
#include <stdlib.h>

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
    ps1_capture c;
    ps1_capture_start(m, &c); m->tty_out = quiet;
    int frames = atoi(argv[2]), i = 0;
    for (; i < frames && !m->exit_requested; i++) ps1_run_frame(m);
    int r = ps1_capture_write(m, &c, argv[3]);
    ps1_free(m);
    return r ? 1 : 0;
}
