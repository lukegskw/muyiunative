// The hardware tests of ps1-tests (tests/ps1-tests/README.md), plan.md's homebrew test programs: each runs under the bundled OpenBIOS, which starts it in place
// of its shell (exe.c), and is checked against what its author recorded on a console. A program that reports on the debug console must print the console's log,
// line for line from its first; one that only draws must leave VRAM as the console did, compared by hash (tools/ps1tests.py makes the expected hash from the
// release's picture of the console's VRAM, and shows a difference). PS1TESTS_DUMP=<folder> writes each drawing program's VRAM there, for tools/ps1tests.py diff
#include "ps1.h"
#include <stdlib.h>
#include <string.h>

#ifndef BIOS_PATH
#error "BIOS_PATH is the bundled OpenBIOS (runtime/tests/Makefile passes it)"
#endif
#ifndef TESTS_DIR
#error "TESTS_DIR is the repository's tests folder (runtime/tests/Makefile passes it)"
#endif

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct { int x, y, w, h; } area;
// frames: a log program's limit, a drawing program's run. extra: lines the release's program prints that its console log predates, from tests added later. Two
// areas are left out: gpu/lines draws two polylines with a fourth colour it never sets (its source sets the third one twice), and outside transparency's 320x240
// picture the console's VRAM holds what was there before
static const struct { const char *exe, *log, *extra; int frames; area at, skip[2]; uint64_t hash; } programs[] = {
    {"cpu/cop/cop.exe", "cpu/cop/psx.log", "", 300, {0}, {{0}}, 0},
    {"cpu/code-in-io/code-in-io.exe", "cpu/code-in-io/psx.log", "", 300, {0}, {{0}}, 0},
    {"gte/test-all/test-all.exe", "gte/test-all/psx.log", "Total tests: 1150\n", 600, {0}, {{0}}, 0},
    {"gpu/gp0-e1/gp0-e1.exe", "gpu/gp0-e1/psx.log", "", 300, {0}, {{0}}, 0},
    {"gpu/mask-bit/mask-bit.exe", "gpu/mask-bit/psx.log", "", 300, {0}, {{0}}, 0},
    {"dma/otc-test/otc-test.exe", "dma/otc-test/psx.log", "pass - testOtcBigTransfer\npass - testOtcControlBitsAfterTransferWithChopping\n", 300, {0}, {{0}}, 0},
    {"dma/dpcr/dpcr.exe", "dma/dpcr/psx.log", "", 300, {0}, {{0}}, 0},
    {"gpu/triangle/triangle.exe", NULL, NULL, 60, {0, 0, VRAM_W, VRAM_H}, {{0}}, 0xa1c558a466507d56},
    {"gpu/quad/quad.exe", NULL, NULL, 60, {0, 0, VRAM_W, VRAM_H}, {{0}}, 0x6f3c652377236b65},
    {"gpu/uv-interpolation/uv-interpolation.exe", NULL, NULL, 60, {0, 0, VRAM_W, VRAM_H}, {{0}}, 0xb61825f4d5bba173},
    {"gpu/rectangles/rectangles.exe", NULL, NULL, 60, {0, 0, VRAM_W, VRAM_H}, {{0}}, 0x36ae789d0f519b54},
    {"gpu/texture-flip/texture-flip.exe", NULL, NULL, 60, {0, 0, VRAM_W, VRAM_H}, {{0}}, 0x714cf7b7ac9b668f},
    {"gpu/texture-overflow/texture-overflow.exe", NULL, NULL, 60, {0, 0, VRAM_W, VRAM_H}, {{0}}, 0x7096f4fdf06e3ed9},
    {"gpu/vram-to-vram-overlap/vram-to-vram-overlap.exe", NULL, NULL, 60, {0, 0, VRAM_W, VRAM_H}, {{0}}, 0x0cf90a98cd6a505d},
    {"gpu/clut-cache/clut-cache.exe", NULL, NULL, 60, {0, 0, VRAM_W, VRAM_H}, {{0}}, 0x76d31cd6324668a7},
    {"gpu/clipping/clipping.exe", NULL, NULL, 60, {0, 0, VRAM_W, VRAM_H}, {{0}}, 0xd2848b88ad6376a5},
    {"gpu/lines/lines.exe", NULL, NULL, 60, {0, 0, VRAM_W, VRAM_H}, {{150, 140, 33, 33}, {210, 140, 33, 33}}, 0x713eb4cec1b8f58f},
    {"gpu/transparency/transparency.exe", NULL, NULL, 60, {0, 0, 320, 240}, {{0}}, 0x42d2ac766c753f25},
};

typedef struct { char **line; int n, cap; } lines;
static void keep(lines *l, const char *s, size_t n) {
    if (l->n == l->cap) { l->cap = l->cap ? 2 * l->cap : 64; l->line = realloc(l->line, sizeof *l->line * (size_t)l->cap); if (!l->line) abort(); }
    char *c = malloc(n + 1); if (!c) abort();
    memcpy(c, s, n); c[n] = 0; l->line[l->n++] = c;
}
static void tty(void *ctx, const char *s) { keep(ctx, s, strlen(s)); }
static void drop(lines *l) { for (int i = 0; i < l->n; i++) free(l->line[i]); free(l->line); }

static char *slurp(const char *rel, size_t *n) {
    char path[512]; snprintf(path, sizeof path, "%s/ps1-tests/%s", TESTS_DIR, rel);
    FILE *f = fopen(path, "rb"); char *b = NULL; long size = -1;
    if (f && !fseek(f, 0, SEEK_END) && (size = ftell(f)) >= 0 && !fseek(f, 0, SEEK_SET) && (b = malloc((size_t)size + 1)) && fread(b, 1, (size_t)size, f) != (size_t)size) size = -1;
    if (f) fclose(f);
    if (size < 0) { free(b); return NULL; }
    b[size] = 0; *n = (size_t)size; return b;
}

// The console's log: CR LF line ends, and in cpu/cop's every line marked "% " by the tool that captured it; empty lines at its end do not count
static void console_log(const char *text, lines *out) {
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && p[n - 1] == '\r') n--;
        if (n >= 2 && p[0] == '%' && p[1] == ' ') { p += 2; n -= 2; }
        keep(out, p, n);
        p = e ? e + 1 : p + strlen(p);
    }
    while (out->n && !*out->line[out->n - 1]) free(out->line[--out->n]);
}

static int first_line(const lines *got, const char *line) { for (int i = 0; i < got->n; i++) if (!strcmp(got->line[i], line)) return i; return -1; }

static int is_extra(const char *extra, const char *line) {   // a whole, non-empty line of extra
    size_t n = strlen(line);
    for (const char *p = extra; n && (p = strstr(p, line)); p++) if ((p == extra || p[-1] == '\n') && p[n] == '\n') return 1;
    return 0;
}

static uint64_t vram_hash(const ps1 *m, area a, const area *skip) {   // tools/ps1tests.py hashes the console's picture the same way
    uint64_t h = 0xcbf29ce484222325;
    for (int y = a.y; y < a.y + a.h; y++)
        for (int x = a.x; x < a.x + a.w; x++) {
            int out = 0;
            for (int k = 0; k < 2; k++) out |= x >= skip[k].x && x < skip[k].x + skip[k].w && y >= skip[k].y && y < skip[k].y + skip[k].h;
            if (!out) h = (h ^ (m->gpu.vram[y * VRAM_W + x] & 0x7fffu)) * 0x100000001b3;
        }
    return h;
}

static void dump(const ps1 *m, const char *exe) {   // named after the program's folder: gpu/lines/lines.exe -> <folder>/gpu-lines.vram
    const char *dir = getenv("PS1TESTS_DUMP");
    if (!dir) return;
    char path[512]; int n = snprintf(path, sizeof path, "%s/", dir);
    for (const char *s = exe; s < strrchr(exe, '/') && n < (int)sizeof path - 6; s++) path[n++] = *s == '/' ? '-' : *s;
    snprintf(path + n, sizeof path - (size_t)n, ".vram");
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(m->gpu.vram, sizeof m->gpu.vram, 1, f) != 1) fprintf(stderr, "ps1-tests: cannot write %s\n", path);
    if (f) fclose(f);
}

int main(void) {
    for (size_t t = 0; t < sizeof programs / sizeof *programs; t++) {
        const char *name = programs[t].exe;
        size_t n = 0; char *exe = slurp(name, &n);
        ps1 *m = ps1_new(); lines got = {0}, want = {0};
        int ok = exe && m && !ps1_load_bios(m, BIOS_PATH) && !ps1_load_exe(m, (uint8_t *)exe, n);
        CHECK(ok, "%s: the program does not load", name);
        char *log = ok && programs[t].log ? slurp(programs[t].log, &n) : NULL;
        if (log) console_log(log, &want);
        CHECK(!ok || !programs[t].log || want.n, "%s: no console log", programs[t].log);
        if (!ok || (programs[t].log && !want.n)) { free(log); free(exe); ps1_free(m); drop(&want); continue; }
        m->tty_out = tty; m->tty_ctx = &got;
        int frames = 0, start = -1, printed = 0;   // printed: lines from the console log's first one on, the extra ones left out
        while (frames < programs[t].frames && !m->exit_requested) {
            ps1_run_frame(m); frames++;
            if (!log || (start = first_line(&got, want.line[0])) < 0) continue;
            printed = 0;
            for (int i = start; i < got.n; i++) printed += !is_extra(programs[t].extra, got.line[i]);
            if (printed >= want.n) break;
        }
        if (log) {
            int i = 0, k = start;
            for (; start >= 0 && i < want.n; i++, k++) {
                while (k < got.n && is_extra(programs[t].extra, got.line[k])) k++;
                if (k >= got.n || strcmp(got.line[k], want.line[i])) break;
            }
            CHECK(i == want.n, "%s: after %d frames, the console's line %d is \"%s\", the program printed \"%s\"", name, frames, i + 1, want.line[i < want.n ? i : 0],
                  start < 0 || k >= got.n ? "(nothing)" : got.line[k]);
            printf("ps1-tests: %s: %d console lines in %d frames\n", name, want.n, frames);
        } else {
            uint64_t h = vram_hash(m, programs[t].at, programs[t].skip);
            dump(m, name);
            CHECK(h == programs[t].hash, "%s: VRAM hash %016llx, the console's %016llx", name, (unsigned long long)h, (unsigned long long)programs[t].hash);
            printf("ps1-tests: %s: VRAM after %d frames, hash %016llx\n", name, frames, (unsigned long long)h);
        }
        drop(&got); drop(&want); free(log); free(exe); ps1_free(m);
    }
    printf("ps1-tests: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
