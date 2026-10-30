// The Mac host (plan.md, phase 1): makes the machine, loads OpenBIOS (bios/openbios; in the app, Contents/Resources/openbios) and runs it on the front end's machine
// thread at the console's speed, each frame shown in the window. --headless runs a number of frames as fast as it can, without a window, prints the last frame's hash and
// can write it as a PNG. --disc mounts an external CUE/BIN read-only (phase 2); it is never copied into the app or the repository
#include <limits.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "front.h"
#include "ps1.h"

enum { MAX_W = 640, MAX_H = 576 };   // the largest picture: 640 pixels by 2 x 288 lines (PAL, 480-line mode)

typedef struct input { unsigned long frame; uint16_t buttons; } input;
typedef struct host { ps1 *m; uint32_t *px; int w, h, interactive; unsigned long frame; input input[64]; int ninput, input_pos; uint16_t buttons; } host;

static void frame(host *h) {
    while (h->input_pos < h->ninput && h->input[h->input_pos].frame <= h->frame) h->buttons = h->input[h->input_pos++].buttons;
    sio_set_pad(h->m, 0, 1, h->interactive ? front_buttons() : h->buttons); h->frame++;
    ps1_run_frame(h->m); ps1_display(h->m, h->px, MAX_W, MAX_H, &h->w, &h->h);
    int16_t samples[2048]; int n;
    while ((n = spu_audio_read(h->m, samples, 1024))) if (h->interactive) front_audio(samples, n);
}

static void run(void *arg) {   // the machine's thread: a frame, shown, then a wait until the console would have finished it
    host *h = arg;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t t0 = mach_absolute_time(), c0 = h->m->cycles, late = 100000000ull * tb.denom / tb.numer;   // 100 ms
    while (!h->m->exit_requested) {
        frame(h); front_present(h->px, h->w, h->h);
        uint64_t due = t0 + (uint64_t)((unsigned __int128)(h->m->cycles - c0) * 1000000000u * tb.denom / ((unsigned __int128)CPU_HZ * tb.numer)), now = mach_absolute_time();
        if (now > due + late) { t0 = now; c0 = h->m->cycles; }   // a stall (the Mac was busy): go on from now instead of rushing to catch up
        else mach_wait_until(due);
    }
    LOG("host: the machine asked to exit (%d)", h->m->exit_code);
}

static const char *default_bios(char *buf, size_t cap) {   // in the app's Resources, else in the repository the host was built in (shell/mac/build)
    char exe[PATH_MAX], real[PATH_MAX]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) || !realpath(exe, real)) return NULL;
    *strrchr(real, '/') = 0;
    snprintf(buf, cap, "%s/../Resources/openbios/openbios.bin", real);
    if (!access(buf, R_OK)) return buf;
    snprintf(buf, cap, "%s/../../../bios/openbios/openbios.bin", real);
    return access(buf, R_OK) ? NULL : buf;
}

static int usage(void) { fputs("usage: ps1native [--bios <file>] [--disc <file.cue|file.bin>] [--headless [--frames <n>] [--input <frame>:<hex-buttons>] [--png <file>]]\n", stderr); return 2; }

int main(int argc, char **argv) {
    const char *bios = NULL, *png = NULL, *disc = NULL;
    input inputs[64]; int ninput = 0;
    int headless = 0; long frames = 300;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { char *end; frames = strtol(argv[++i], &end, 10); if (*end || frames < 1) return usage(); }
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            const char *s = argv[++i]; char *end; errno = 0;
            unsigned long frame = strtoul(s, &end, 10);
            if (ninput == 64 || errno || end == s || *end != ':' || frame > LONG_MAX) return usage();
            s = end + 1; errno = 0; unsigned long buttons = strtoul(s, &end, 16);
            if (errno || end == s || *end || buttons > 0xffff || (ninput && frame < inputs[ninput - 1].frame)) return usage();
            inputs[ninput++] = (input){frame, (uint16_t)buttons};
        }
        else return usage();
    }
    if (ninput && !headless) return usage();
    char path[PATH_MAX];
    if (!bios && !(bios = default_bios(path, sizeof path))) { RED("host: no BIOS in the app's Resources or the repository's bios/openbios: give one with --bios"); return 1; }
    host h = {.m = ps1_new(), .px = malloc(sizeof *h.px * MAX_W * MAX_H), .interactive = !headless, .ninput = ninput};
    memcpy(h.input, inputs, (size_t)ninput * sizeof *inputs);
    if (!h.m || !h.px) { RED("host: no memory for the machine"); free(h.px); ps1_free(h.m); return 1; }
    if (ps1_load_bios(h.m, bios) || (disc && disc_open(h.m, disc))) { free(h.px); ps1_free(h.m); return 1; }
    if (disc) LOG("host: external disc mounted read-only");
    LOG("host: BIOS %s", bios);
    if (!headless) { front_run("ps1native", run, &h); return 0; }
    long n = 0;
    while (n < frames && !h.m->exit_requested) { frame(&h); n++; }
    printf("host: %ld frames, %dx%d, frame hash %016llx\n", n, h.w, h.h, (unsigned long long)ps1_frame_hash(h.px, h.w, h.h));
    int r = h.m->exit_requested ? h.m->exit_code : png && h.w && front_write_png(png, h.px, h.w, h.h) ? 1 : 0;
    free(h.px); ps1_free(h.m);
    return r;
}
