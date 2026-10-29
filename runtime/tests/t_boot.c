// Phase 1's exit criterion without a window: the bundled OpenBIOS (bios/openbios) boots to its shell, which turns the display on in 640x480 interlaced mode and draws its
// cube over a background (more than one colour). The frame's hash is printed, not checked: the shell animates, so it changes with any change of timing. The [RED] lines
// are what the shell uses that is not modelled yet
#include "ps1.h"
#include <stdlib.h>

#ifndef BIOS_PATH
#error "BIOS_PATH is the bundled OpenBIOS (runtime/tests/Makefile passes it)"
#endif

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

int main(void) {
    enum { FRAMES = 300, W = 640, H = 480 };
    ps1 *m = ps1_new();
    uint32_t *px = malloc(sizeof *px * W * H);
    if (!m || !px) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }
    if (ps1_load_bios(m, BIOS_PATH)) { fputs("FAIL: no BIOS at " BIOS_PATH "\n", stderr); return 1; }
    int frames = 0;
    while (frames < FRAMES && !m->exit_requested) { ps1_run_frame(m); frames++; }
    int w, h, colours = 1;
    ps1_display(m, px, W, H, &w, &h);
    for (int i = 1; i < w * h && colours == 1; i++) colours += px[i] != px[0];
    CHECK(frames == FRAMES, "the BIOS runs without asking to exit");
    CHECK(!m->gpu.display_off, "the display is on");
    CHECK((m->gpu.dmode & 0x3f) == 0x27 && w == W && h == H, "the shell's mode: 640x480, interlaced, NTSC, 15-bit");
    CHECK(colours > 1, "the picture is not one colour");
    printf("boot: %d frames, pc %08x, display %dx%d, frame hash %016llx\n", frames, m->cpu.pc, w, h, (unsigned long long)ps1_frame_hash(px, w, h));
    printf("boot: %d checks, %d failed\n", checks, fails);
    free(px); ps1_free(m);
    return fails != 0;
}
