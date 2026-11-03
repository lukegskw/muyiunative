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
#include "product.h"
#include "saves.h"
#include "state.h"
#include "ps1.h"
#include "renderer.h"
#include "report.h"

enum { MAX_W = 640, MAX_H = 576 };   // the largest picture: 640 pixels by 2 x 288 lines (PAL, 480-line mode)

#include "input.h"
typedef struct host {
    ps1 *m, *ref; ps1_lockstep ls; int diverged;   // ref: the interpreting machine of --lockstep
    uint32_t *px; int w, h, interactive; unsigned long frame; input *input; size_t ninput, input_pos; uint16_t buttons;
    front_saves saves; int saving; const char *state_game;
    metal_renderer *renderer; const char *shaders; int choice, capscale, verify_renderer, graphics_failed;
} host;
extern const ps1_native_set native_openbios;
#ifdef NATIVE_GAME
extern const ps1_native_set native_game;
#endif

static void drain(ps1 *m, int play) { int16_t samples[2048]; int n; while ((n = spu_audio_read(m, samples, 1024))) if (play) front_audio(samples, n); }

static int quick_state_data(host *h, int action, char error[FRONT_STATE_ERROR]) {
    if (action == FRONT_STATE_SAVE) {
        int scale = h->renderer && h->choice > 1 ? h->choice : 0;
        size_t words = (size_t)VRAM_W * VRAM_H * scale * scale;
        uint16_t *fine = words ? malloc(words * sizeof *fine) : NULL;
        if (words && (!fine || metal_renderer_save_state(h->renderer, fine, words))) {
            free(fine); snprintf(error, FRONT_STATE_ERROR, "Could not capture the Metal frame for this state."); return -1;
        }
        int result = front_state_save(h->saves.dir_fd, h->state_game, h->m, h->frame, scale, fine, error); free(fine); return result;
    }
    front_snapshot *s = front_state_read(h->saves.dir_fd, h->state_game, h->m, error); if (!s) return -1;
    metal_renderer_free(h->renderer); h->renderer = NULL; front_state_apply(h->m, s); h->frame = (unsigned long)s->frame;
    if (h->interactive) h->choice = front_renderer();
    if (h->choice) {
        h->renderer = metal_renderer_new(h->m, h->shaders, h->choice);
        if (h->renderer && s->fine && metal_renderer_load_state(h->renderer, s->fine, s->scale)) {
            metal_renderer_free(h->renderer); h->renderer = NULL;
        }
        if (!h->renderer) { RED("host: cannot restore Metal; using software"); h->choice = 0; front_renderer_set(0); }
    }
    front_state_free(s);
    if (h->renderer) {
        if (metal_renderer_frame(h->renderer, h->px, MAX_W * h->capscale, MAX_H * h->capscale, &h->w, &h->h, 0)) {
            snprintf(error, FRONT_STATE_ERROR, "The state loaded, but Metal could not display it."); h->graphics_failed = 1; return -1;
        }
    } else ps1_display(h->m, h->px, MAX_W, MAX_H, &h->w, &h->h);
    if (h->interactive) { front_audio_reset(); front_present(h->px, h->w, h->h); }
    return 0;
}

static int quick_state(host *h, int action, char error[FRONT_STATE_ERROR]) {
    if (h->interactive && front_audio_pause(1)) {
        snprintf(error, FRONT_STATE_ERROR, "Could not pause audio for this state operation."); return -1;
    }
    int result = quick_state_data(h, action, error);
    // Saving and failed loads keep the same playback timeline. Successful loads replace it while paused. Resume on every path, including file/GPU errors.
    if (h->interactive && front_audio_pause(0) && !result) {
        snprintf(error, FRONT_STATE_ERROR, "The state operation finished, but audio could not resume."); return -1;
    }
    return result;
}

static void frame(host *h) {
    if (h->interactive && front_renderer() != h->choice) {
        h->choice = front_renderer(); metal_renderer_free(h->renderer); h->renderer = NULL;
        if (h->choice) h->renderer = metal_renderer_new(h->m, h->shaders, h->choice);
        if (h->choice && !h->renderer) { RED("host: Metal could not start; keeping the software renderer"); h->choice = 0; front_renderer_set(0); }
    }
    while (h->input_pos < h->ninput && h->input[h->input_pos].frame <= h->frame) h->buttons = h->input[h->input_pos++].buttons;
    front_pad pads[2] = {{1, h->buttons}, {0, 0}};
    if (h->interactive) front_buttons(pads);
    for (int port = 0; port < 2; port++) sio_set_pad(h->m, port, pads[port].connected, pads[port].buttons);
    h->frame++;
    if (h->ref) {   // both machines take the same input and give up the same sound, so that they stay comparable
        for (int port = 0; port < 2; port++) sio_set_pad(h->ref, port, pads[port].connected, pads[port].buttons);
        if (ps1_lockstep_frame(h->ref, h->m, &h->ls)) { RED("host: lockstep, frame %lu: %s", h->frame - 1, h->ls.report); h->diverged = 1; }
        drain(h->ref, 0);
    } else ps1_run_frame(h->m);
    // AudioQueue missed 167 sample frames in a paced results scene although samples were ready ~10 ms before the Metal wait ended. Publish produced audio now;
    // presentation does not affect SPU output. Buffer sizes and guest timing stay unchanged, and the reference above drains its own copy at the same boundary.
    drain(h->m, h->interactive);
    if (h->renderer) {
        int result = h->interactive && !h->verify_renderer ?
            metal_renderer_frame_async(h->renderer, h->px, MAX_W * h->capscale, MAX_H * h->capscale, &h->w, &h->h) :
            metal_renderer_frame(h->renderer, h->px, MAX_W * h->capscale, MAX_H * h->capscale, &h->w, &h->h, h->verify_renderer);
        if (result) {
            RED("host: renderer failed at frame %lu", h->frame - 1); h->graphics_failed = 1;
        }
    } else ps1_display(h->m, h->px, MAX_W, MAX_H, &h->w, &h->h);
}

static void run(void *arg) {   // the machine's thread: a frame, shown, then a wait until the console would have finished it
    host *h = arg;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t t0 = mach_absolute_time(), c0 = h->m->cycles, late = 100000000ull * tb.denom / tb.numer;   // 100 ms
    uint64_t saved = h->m->cycles;
    for (;;) {
      while (!h->m->exit_requested && !front_stopping() && !h->graphics_failed) {
        int action = front_state_request();
        if (action) {
            char error[FRONT_STATE_ERROR]; int result = quick_state(h, action, error);
            if (result) RED("state: %s", error); else LOG("state: %s at frame %lu", action == FRONT_STATE_SAVE ? "saved" : "loaded", h->frame);
            front_state_complete(action, result ? error : NULL);
            // Restoring rewinds guest cycles. Rebase both pacing and the card-flush deadline; file I/O pauses do not become catch-up frames.
            t0 = mach_absolute_time(); c0 = saved = h->m->cycles;
        }
        frame(h); front_present(h->px, h->w, h->h);
        if (h->saving && h->m->cycles - saved >= CPU_HZ) { front_saves_flush(&h->saves, h->m); saved = h->m->cycles; }
        uint64_t due = t0 + (uint64_t)((unsigned __int128)(h->m->cycles - c0) * 1000000000u * tb.denom / ((unsigned __int128)CPU_HZ * tb.numer)), now = mach_absolute_time();
        if (now > due + late) { t0 = now; c0 = h->m->cycles; }   // a stall (the Mac was busy): go on from now instead of rushing to catch up
        else mach_wait_until(due);
      }
      if (!h->saving || !front_saves_flush(&h->saves, h->m)) break;
      front_cancel_stop("The memory cards could not be saved. The game remains open so you can fix the save folder's permissions or free disk space, then quit again.");
      if (h->m->exit_requested || h->graphics_failed) while (!front_stopping()) usleep(10000);
    }
    front_saves_close(&h->saves);
    metal_renderer_free(h->renderer); h->renderer = NULL;
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
    fputs("usage: muyiunative [--bios <file>] [--disc <file.cue|file.bin>] [--exe <file.exe>] [--interpret] [--save-dir <directory>] "
          "[--renderer software|metal] [--scale 1|2|4] [--verify-renderer] [--shaders <offline.metallib>] "
          "[--headless [--frames <n>] [--input <frame>:<hex-buttons>] [--input-file <file>] [--png <file>] [--lockstep | --capture <out>]]\n", stderr);
    return 2;
}

static void quiet(void *ctx, const char *line) { (void)ctx; (void)line; }

static const char *default_shaders(char *buf, size_t cap) {
    char exe[PATH_MAX], real[PATH_MAX]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) || !realpath(exe, real)) return NULL;
    *strrchr(real, '/') = 0;
    const char *locations[] = {"../Resources/ps1gpu.metallib", "gen/ps1gpu.metallib", "../gen/ps1gpu.metallib"};
    for (size_t i = 0; i < sizeof locations / sizeof *locations; i++) { snprintf(buf, cap, "%s/%s", real, locations[i]); if (!access(buf, R_OK)) return buf; }
    return NULL;
}

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
    const char *bios = NULL, *png = NULL, *disc = NULL, *exe = NULL, *capture = NULL, *save_dir = NULL;
    const char *shaders = NULL; int renderer = -1, scale = 0, verify_renderer = 0;
    input *inputs = NULL; size_t ninput = 0, input_cap = 0;
    int headless = 0, interpret = 0, lockstep = 0; long frames = 300;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bios") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc) save_dir = argv[++i];
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) { const char *r = argv[++i]; if (!strcmp(r, "metal")) renderer = 1; else if (!strcmp(r, "software")) renderer = 0; else return usage(); }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) { const char *s = argv[++i]; if (!strcmp(s, "1")) scale = 1; else if (!strcmp(s, "2")) scale = 2; else if (!strcmp(s, "4")) scale = 4; else return usage(); }
        else if (!strcmp(argv[i], "--verify-renderer")) verify_renderer = 1;
        else if (!strcmp(argv[i], "--shaders") && i + 1 < argc) shaders = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--interpret")) interpret = 1;
        else if (!strcmp(argv[i], "--lockstep")) lockstep = 1;
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { char *end; frames = strtol(argv[++i], &end, 10); if (*end || frames < 1) return usage(); }
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) { if (front_input_add(&inputs, &ninput, &input_cap, argv[++i])) { free(inputs); return usage(); } }
        else if (!strcmp(argv[i], "--input-file") && i + 1 < argc) { if (front_input_file(&inputs, &ninput, &input_cap, argv[++i])) { free(inputs); return usage(); } }
        else if (strncmp(argv[i], "-psn_", 5)) return usage();   // Finder's legacy process-serial argument
    }
    if ((ninput || lockstep || capture) && !headless) return usage();
    if (lockstep && (interpret || capture)) return usage();
    front_reports_start(headless);
    front_product product = {0}; int bundled = front_product_load(&product);
    if (bundled < 0) { if (!headless) front_error("This game app is incomplete. Build it again from your disc."); return 1; }
    if (disc || exe) bundled = 0; else if (bundled) disc = product.disc;
    if (!headless && !disc && !exe && !bios) { free(inputs); front_launcher(); return 0; }
    front_reports_game(bundled ? product.serial : "");
    if (renderer < 0) renderer = !headless && bundled && product.render_scale;
    if (!scale) scale = renderer && !headless && bundled && product.render_scale ? product.render_scale : 1;
    if (!renderer && (scale != 1 || verify_renderer)) return usage();
    char path[PATH_MAX], save_path[PATH_MAX], shader_path[PATH_MAX];
    if (!shaders) shaders = default_shaders(shader_path, sizeof shader_path);
    if (bundled && !headless && !save_dir) {
        if (front_save_directory(save_path, sizeof save_path, product.save_id)) { front_error("Cannot locate Application Support for your memory cards."); return 1; }
        save_dir = save_path;
    }
    if (!bios && !(bios = default_bios(path, sizeof path))) { RED("host: no BIOS in the app's Resources or the repository's bios/openbios: give one with --bios"); return 1; }
    int capscale = headless ? (renderer ? scale : 1) : 4;
    host h = {.m = ps1_new(), .ref = lockstep ? ps1_new() : NULL, .ls = {.step = 64}, .px = malloc(sizeof *h.px * MAX_W * MAX_H * capscale * capscale), .interactive = !headless,
              .ninput = ninput, .input = inputs, .saves = {-1, -1}, .state_game = bundled ? product.save_id : NULL,
              .shaders = shaders, .choice = renderer ? scale : 0, .capscale = capscale, .verify_renderer = verify_renderer};
    if (!h.m || !h.px || (lockstep && !h.ref)) { RED("host: no memory for the machine"); free(h.input); free(h.px); ps1_free(h.m); ps1_free(h.ref); return 1; }
    if (ps1_load_bios(h.m, bios) || (disc && disc_open(h.m, disc)) || (exe && load_exe(h.m, exe)) ||
        (h.ref && (ps1_load_bios(h.ref, bios) || (disc && disc_open(h.ref, disc)) || (exe && load_exe(h.ref, exe))))) {
        free(h.input); free(h.px); ps1_free(h.m); ps1_free(h.ref); return 1;
    }
    if (h.ref) h.ref->tty_out = quiet;   // the native machine prints what both do
    if (!headless) h.m->tty_out = front_reports_tty;   // host-only reporting policy; headless validation retains its exact output
    if (save_dir) {
        if (front_saves_open(&h.saves, h.m, save_dir)) {
            if (!headless) front_error("Cannot open the memory cards. Close any other copy of this game, and check that its save folder is writable and its cards are valid.");
            free(h.input); free(h.px); ps1_free(h.m); ps1_free(h.ref); return 1;
        }
        h.saving = 1;
        if (h.ref) memcpy(h.ref->sio.card, h.m->sio.card, sizeof h.m->sio.card);   // initial state only; guest writes remain independently checked
    }
    if (!interpret) ps1_native_add(h.m, &native_openbios);
#ifdef NATIVE_GAME
    if (!interpret && !capture) ps1_native_add(h.m, &native_game);
#endif
    if (renderer && !(h.renderer = metal_renderer_new(h.m, shaders, scale))) {
        front_saves_close(&h.saves); free(h.input); free(h.px); ps1_free(h.m); ps1_free(h.ref); return 1;
    }
    ps1_capture cap;
    if (capture) ps1_capture_start(h.m, &cap);
    if (disc) LOG("host: external disc mounted read-only");
    LOG("host: BIOS %s%s%s", bios, interpret ? ", interpreted" : ", OpenBIOS as native code", lockstep ? ", in lockstep with the interpreter" : "");
    if (!headless) {
        front_renderer_set(h.choice); front_state_menu(h.state_game && h.saving, h.saving && front_state_exists(h.saves.dir_fd));
        front_run(bundled ? product.title : "muyiunative", run, &h); return 0;
    }
    long n = 0;
    while (n < frames && !h.m->exit_requested && !h.diverged && !h.graphics_failed) { frame(&h); n++; }
    printf("host: %ld frames, %dx%d, frame hash %016llx\n", n, h.w, h.h, (unsigned long long)ps1_frame_hash(h.px, h.w, h.h));
    printf("host: native code entered %llu times; %llu instructions interpreted\n", (unsigned long long)h.m->native_runs, (unsigned long long)h.m->interp_steps);
    printf("host: %llu instructions native, %llu interpreted (%.2f%% native)\n", (unsigned long long)h.m->native_steps,
           (unsigned long long)h.m->interp_steps, 100.0 * h.m->native_steps / (h.m->native_steps + h.m->interp_steps + 1));
    if (h.ref) printf("host: lockstep: %s, %llu stops\n", h.diverged ? "the machines differ" : "the machines agree", (unsigned long long)h.ls.stops);
    if (h.renderer) printf("host: Metal renderer, %dx, %ld frames%s\n", scale, n, verify_renderer && !h.graphics_failed ? ", native VRAM/cache agree with software every frame" : "");
    int r = h.diverged ? 3 : h.graphics_failed ? 4 : h.m->exit_requested ? h.m->exit_code : png && h.w && front_write_png(png, h.px, h.w, h.h) ? 1 : 0;
    if (capture && ps1_capture_write(h.m, &cap, capture)) r = 1;
    // A failed lockstep comparison can include rejected card writes; preserve the last good save and divergence exit code.
    if (h.saving && !h.diverged && !h.graphics_failed && front_saves_flush(&h.saves, h.m)) r = 1;
    front_saves_close(&h.saves);
    metal_renderer_free(h.renderer); free(h.input); free(h.px); ps1_free(h.m); ps1_free(h.ref);
    return r;
}
