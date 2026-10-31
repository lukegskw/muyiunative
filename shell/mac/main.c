// The Mac host (plan.md, phase 1): makes the machine, loads OpenBIOS (bios/openbios; in the app, Contents/Resources/openbios) and runs it on the front end's machine
// thread at the console's speed, each frame shown in the window. --headless runs a number of frames as fast as it can, without a window, prints the last frame's hash and
// can write it as a PNG. --disc mounts an external CUE/BIN read-only (phase 2); it is never copied into the app or the repository. OpenBIOS runs as native code, built
// with the host from the bundled binary (phase 3), where memory holds the code it was made from; --interpret runs everything in the interpreter instead, and
// --lockstep (headless) runs an interpreting machine beside the native one and stops at their first difference (runtime/lockstep.c). A host built with a game's native
// code (make -C shell/mac game, scripts/native_game.py) runs that too. --capture (headless) keeps what the interpreted code calls and jumps to, and RAM, for
// tools/codegen.py, including code versions overwritten later (phase 4); it leaves out the game's native code, so the capture sees what the game runs
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

#include "input.h"
typedef struct host {
    ps1 *m, *ref; ps1_lockstep ls; int diverged;   // ref: the interpreting machine of --lockstep
    uint32_t *px; int w, h, interactive; unsigned long frame; input *input; size_t ninput, input_pos; uint16_t buttons;
} host;
extern const ps1_native_set native_openbios;
#ifdef NATIVE_GAME
extern const ps1_native_set native_game;
#endif

static void drain(ps1 *m, int play) { int16_t samples[2048]; int n; while ((n = spu_audio_read(m, samples, 1024))) if (play) front_audio(samples, n); }

static void frame(host *h) {
    while (h->input_pos < h->ninput && h->input[h->input_pos].frame <= h->frame) h->buttons = h->input[h->input_pos++].buttons;
    uint16_t b = h->interactive ? front_buttons() : h->buttons;
    sio_set_pad(h->m, 0, 1, b); h->frame++;
    if (h->ref) {   // both machines take the same input and give up the same sound, so that they stay comparable
        sio_set_pad(h->ref, 0, 1, b);
        if (ps1_lockstep_frame(h->ref, h->m, &h->ls)) { RED("host: lockstep, frame %lu: %s", h->frame - 1, h->ls.report); h->diverged = 1; }
        drain(h->ref, 0);
    } else ps1_run_frame(h->m);
    ps1_display(h->m, h->px, MAX_W, MAX_H, &h->w, &h->h);
    drain(h->m, h->interactive);
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

static const char *default_bios(char *buf, size_t cap) {   // in the app's Resources, else in the repository the host was built in (shell/mac/build, or build/game)
    char exe[PATH_MAX], real[PATH_MAX]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) || !realpath(exe, real)) return NULL;
    *strrchr(real, '/') = 0;
    snprintf(buf, cap, "%s/../Resources/openbios/openbios.bin", real);
    if (!access(buf, R_OK)) return buf;
    snprintf(buf, cap, "%s/../../../bios/openbios/openbios.bin", real);
    if (!access(buf, R_OK)) return buf;
    snprintf(buf, cap, "%s/../../../../bios/openbios/openbios.bin", real);
    return access(buf, R_OK) ? NULL : buf;
}

static int usage(void) {
    fputs("usage: ps1native [--bios <file>] [--disc <file.cue|file.bin>] [--exe <file.exe>] [--interpret] "
          "[--headless [--frames <n>] [--input <frame>:<hex-buttons>] [--input-file <file>] [--png <file>] [--lockstep | --capture <out>]]\n", stderr);
    return 2;
}

static void quiet(void *ctx, const char *line) { (void)ctx; (void)line; }

static int load_exe(ps1 *m, const char *path) {   // the whole file, handed to the runtime to start in place of the BIOS shell
    FILE *f = fopen(path, "rb"); uint8_t *b = NULL; long n = -1;
    if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) && (b = malloc((size_t)n)) && fread(b, 1, (size_t)n, f) != (size_t)n) n = -1;
    if (f) fclose(f);
    int r = -1;
    if (n > 0 && b) r = ps1_load_exe(m, b, (size_t)n); else RED("host: cannot read %s", path);
    free(b);
    return r;
}

int main(int argc, char **argv) {
    const char *bios = NULL, *png = NULL, *disc = NULL, *exe = NULL, *capture = NULL;
    input *inputs = NULL; size_t ninput = 0, input_cap = 0;
    int headless = 0, interpret = 0, lockstep = 0; long frames = 300;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--interpret")) interpret = 1;
        else if (!strcmp(argv[i], "--lockstep")) lockstep = 1;
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { char *end; frames = strtol(argv[++i], &end, 10); if (*end || frames < 1) return usage(); }
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) { if (front_input_add(&inputs, &ninput, &input_cap, argv[++i])) { free(inputs); return usage(); } }
        else if (!strcmp(argv[i], "--input-file") && i + 1 < argc) { if (front_input_file(&inputs, &ninput, &input_cap, argv[++i])) { free(inputs); return usage(); } }
        else return usage();
    }
    if ((ninput || lockstep || capture) && !headless) return usage();
    if (lockstep && (interpret || capture)) return usage();
    char path[PATH_MAX];
    if (!bios && !(bios = default_bios(path, sizeof path))) { RED("host: no BIOS in the app's Resources or the repository's bios/openbios: give one with --bios"); return 1; }
    host h = {.m = ps1_new(), .ref = lockstep ? ps1_new() : NULL, .ls = {.step = 64}, .px = malloc(sizeof *h.px * MAX_W * MAX_H), .interactive = !headless,
              .ninput = ninput, .input = inputs};
    if (!h.m || !h.px || (lockstep && !h.ref)) { RED("host: no memory for the machine"); free(h.input); free(h.px); ps1_free(h.m); ps1_free(h.ref); return 1; }
    if (ps1_load_bios(h.m, bios) || (disc && disc_open(h.m, disc)) || (exe && load_exe(h.m, exe)) ||
        (h.ref && (ps1_load_bios(h.ref, bios) || (disc && disc_open(h.ref, disc)) || (exe && load_exe(h.ref, exe))))) {
        free(h.input); free(h.px); ps1_free(h.m); ps1_free(h.ref); return 1;
    }
    if (h.ref) h.ref->tty_out = quiet;   // the native machine prints what both do
    if (!interpret) ps1_native_add(h.m, &native_openbios);
#ifdef NATIVE_GAME
    if (!interpret && !capture) ps1_native_add(h.m, &native_game);
#endif
    ps1_capture cap;
    if (capture) ps1_capture_start(h.m, &cap);
    if (disc) LOG("host: external disc mounted read-only");
    LOG("host: BIOS %s%s%s", bios, interpret ? ", interpreted" : ", OpenBIOS as native code", lockstep ? ", in lockstep with the interpreter" : "");
    if (!headless) { front_run("ps1native", run, &h); return 0; }
    long n = 0;
    while (n < frames && !h.m->exit_requested && !h.diverged) { frame(&h); n++; }
    printf("host: %ld frames, %dx%d, frame hash %016llx\n", n, h.w, h.h, (unsigned long long)ps1_frame_hash(h.px, h.w, h.h));
    printf("host: native code entered %llu times; %llu instructions interpreted\n", (unsigned long long)h.m->native_runs, (unsigned long long)h.m->interp_steps);
    printf("host: %llu instructions native, %llu interpreted (%.2f%% native)\n", (unsigned long long)h.m->native_steps,
           (unsigned long long)h.m->interp_steps, 100.0 * h.m->native_steps / (h.m->native_steps + h.m->interp_steps + 1));
    if (h.ref) printf("host: lockstep: %s, %llu stops\n", h.diverged ? "the machines differ" : "the machines agree", (unsigned long long)h.ls.stops);
    int r = h.diverged ? 3 : h.m->exit_requested ? h.m->exit_code : png && h.w && front_write_png(png, h.px, h.w, h.h) ? 1 : 0;
    if (capture && ps1_capture_write(h.m, &cap, capture)) r = 1;
    free(h.input); free(h.px); ps1_free(h.m); ps1_free(h.ref);
    return r;
}
