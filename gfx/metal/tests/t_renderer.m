// Phase 6's Metal differential test: synthetic GP0/GP1 streams exercise independent native VRAM/cache and display output against gpu_sw.c. No game, SDK,
// retail BIOS, window or screenshots. The GPU test complements CPU-only tests locally and in CI; a missing Metal device is reported as skipped, never as a pass.
#import <Metal/Metal.h>
#include "../renderer.h"
#include <stdlib.h>
#include <string.h>

#ifndef METALLIB_PATH
#error "METALLIB_PATH names the offline-compiled renderer library"
#endif
static int checks, fails, packets;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define V(x,y) ((uint32_t)(uint16_t)(y) << 16 | (uint16_t)(x))
#define UV(u,v,a) ((uint32_t)(a) << 16 | (uint32_t)(v) << 8 | (uint32_t)(u))
typedef struct fixture { ps1 *m; metal_renderer *r; int scale; } fixture;
static void equal(fixture *t) { CHECK(!metal_renderer_check(t->r), "packet %d matches native VRAM and texture/CLUT cache", ++packets); }
static void words(fixture *t, const uint32_t *w, size_t n) {
    for (size_t i = 0; i < n; i++) { gpu_gp0(t->m, w[i]); if (!t->m->gpu.n && !t->m->gpu.poly && t->m->gpu.xfer != 1) equal(t); }
}
#define GP0(t, ...) do { const uint32_t w_[] = {__VA_ARGS__}; words(t, w_, sizeof w_ / sizeof *w_); } while (0)
#define BATCH(t, ...) do { const uint32_t w_[] = {__VA_ARGS__}; for (size_t i_ = 0; i_ < sizeof w_ / sizeof *w_; i_++) gpu_gp0((t)->m, w_[i_]); } while (0)
static fixture create(int scale) {
    fixture t = {.m = ps1_new(), .scale = scale};
    if (t.m) t.r = metal_renderer_new(t.m, METALLIB_PATH, scale);
    CHECK(t.m && t.r, "a separate %dx renderer attaches to its machine", scale); return t;
}
static void drop(fixture *t) { metal_renderer_free(t->r); ps1_free(t->m); *t = (fixture){0}; }
static void drawing(fixture *t) {
    GP0(t, 0xe1000000, 0xe2000000, 0xe3000000, 0xe4000000 | 1023 | 511 << 10, 0xe5000000, 0xe6000000);
    gpu_gp1(t->m, 0x03000000); gpu_gp1(t->m, 0x05000000); gpu_gp1(t->m, 0x07040010); gpu_gp1(t->m, 0x08000001);
}
static void upload(fixture *t, int x, int y, const uint16_t *data, unsigned n) {
    gpu_gp0(t->m, 0xa0000000); gpu_gp0(t->m, V(x, y)); gpu_gp0(t->m, V(n, 1));
    for (unsigned i = 0; i < n; i += 2) gpu_gp0(t->m, data[i] | (uint32_t)(i + 1 < n ? data[i + 1] : 0xdead) << 16);
    equal(t);
}
static void pixel(fixture *t, int x, int y, uint16_t color) { upload(t, x, y, &color, 1); }
static void sprite(fixture *t, uint32_t cmd, int x, int y, int u, int v, unsigned clut, int w, int h) {
    GP0(t, cmd, V(x, y), UV(u, v, clut), V(w, h));
}
static void frame(fixture *t) {
    int cap_w = 640 * t->scale, cap_h = 576 * t->scale, w = 0, h = 0, sw = 0, sh = 0;
    uint32_t *out = calloc((size_t)cap_w * cap_h, sizeof *out), *ref = calloc(640 * 576, sizeof *ref);
    CHECK(out && ref, "frame buffers allocate"); if (!out || !ref) { free(out); free(ref); return; }
    CHECK(!metal_renderer_frame(t->r, out, cap_w, cap_h, &w, &h, 1), "GPU frame synchronizes and verifies");
    ps1_display(t->m, ref, 640, 576, &sw, &sh);
    CHECK(w == sw * t->scale && h == sh * t->scale, "%dx frame dimensions: %dx%d from native %dx%d", t->scale, w, h, sw, sh);
    if (t->scale == 1 && w == sw && h == sh) CHECK(!memcmp(out, ref, (size_t)w * h * sizeof *out), "the complete native-resolution BGRA frame matches software");
    free(out); free(ref);
}

static void primitives(fixture *t) {
    drawing(t);
    GP0(t, 0x024080c0, V(0x13, 2), V(0x11, 3));
    GP0(t, 0x28ffffff, V(100, 100), V(104, 100), V(100, 104), V(104, 104));
    GP0(t, 0x20ff00ff, V(120, 100), V(124, 100), V(120, 104));
    GP0(t, 0x300000ff, V(0, 300), 0x0000ff00, V(40, 300), 0x00ff0000, V(0, 340));
    GP0(t, 0x380000ff, V(160, 100), 0x0000ff00, V(180, 100), 0x00ff0000, V(160, 120), 0x00ffffff, V(180, 120));
    GP0(t, 0xe3000000 | 200 | 200 << 10, 0xe4000000 | 203 | 201 << 10, 0xe5000000 | 10 | 20 << 11);
    GP0(t, 0x60ffffff, V(185, 175), V(20, 20));
    drawing(t);
    GP0(t, 0xe5000000 | 0x7ff | 0x7fe << 11, 0x20ffffff, V(-2, -2), V(15, -2), V(-2, 15));
    drawing(t);
    GP0(t, 0x20ffffff, V(-500, 5), V(600, 5), V(0, 10));
    GP0(t, 0x20ffffff, V(1, 1), V(2, 2), V(3, 3));
    for (unsigned octant = 0; octant < 8; octant++) {
        int dx = octant & 1 ? -13 : 13, dy = octant & 2 ? -5 : 5;
        if (octant & 4) { int tmp = dx; dx = dy; dy = tmp; }
        GP0(t, 0x500000ff, V(300, 200), 0x00ff0000, V(300 + dx, 200 + dy));
    }
    GP0(t, 0xe1000200, 0x4000ff00, V(300, 10), V(303, 10));
    GP0(t, 0x4000ff00, V(310, 10), V(310, 10));
    GP0(t, 0x480000ff, V(300, 20), V(302, 20), V(302, 22), 0x55555555);
    GP0(t, 0x580000ff, V(310, 20), 0x0000ff00, V(312, 20), 0x00ff0000, V(312, 22), 0x55555555);
    for (unsigned mode = 0; mode < 4; mode++) {
        GP0(t, 0x02404040, V(400, 40), V(16, 4), 0xe1000000 | mode << 5);
        GP0(t, 0x62505050, V(400, 40), V(8, 2));
    }
    GP0(t, 0xe6000001, 0x68ffffff, V(500, 40), 0xe6000002, 0x680000ff, V(500, 40));
    GP0(t, 0x0200ff00, V(496, 40), V(16, 1));
    drawing(t); frame(t);
}

static void transfers(fixture *t) {
    const uint16_t data[] = {0x1111, 0x8222, 0x3333, 0x4444, 0x5555};
    upload(t, 1022, 511, data, 5);
    GP0(t, 0xc0000000, V(1022, 511), V(5, 1));
    CHECK(gpu_read(t->m) == 0x82221111 && gpu_read(t->m) == 0x44443333 && gpu_read(t->m) == 0x5555, "odd readback and wrapped upload retain the exact five words");
    equal(t);
    GP0(t, 0x80000000, V(1022, 511), V(600, 200), V(5, 1));
    upload(t, 600, 210, data, 5); GP0(t, 0x80000000, V(600, 210), V(601, 210), V(5, 1));
    GP0(t, 0x80000000, V(600, 210), V(600, 211), V(6, 3));
    GP0(t, 0x80000000, V(600, 211), V(600, 210), V(6, 3));
    GP0(t, 0xe6000002); pixel(t, 601, 200, 0x1234); GP0(t, 0xe6000000);
    gpu_gp0(t->m, 0x20ffffff); gpu_gp0(t->m, V(0, 0)); equal(t);
    gpu_gp1(t->m, 0x01000000); equal(t);
    GP0(t, 0x68ffffff, V(520, 40));
    gpu_gp1(t->m, 0x00000000); equal(t);
    const uint16_t *readback = metal_renderer_vram(t->r);
    CHECK(readback && readback[40 * VRAM_W + 520] == 0x7fff, "GP1 reset preserves VRAM");
    drawing(t); frame(t);
}

static void textures(fixture *t) {
    uint16_t palette[256], source[64]; unsigned clut = 20 | 300 << 6;
    for (unsigned i = 0; i < 256; i++) palette[i] = (uint16_t)(i ? i | 0x8000 : 0);
    upload(t, 320, 300, palette, 256);
    for (unsigned depth = 0; depth < 4; depth++) {
        drawing(t); GP0(t, 0xe1000000 | depth << 7);
        for (unsigned i = 0; i < 64; i++) source[i] = depth == 0 ? 0x1111 : depth == 1 ? 0x0101 : 0x801f;
        upload(t, 0, 0, source, 64); upload(t, 0, depth < 2 ? 64 : 32, source, 64);
        GP0(t, 0x01000000);
        sprite(t, 0x65000000, 400, 60 + depth * 6, 0, 0, clut, 4, 2);
        pixel(t, 0, 0, depth == 0 ? 0x2222 : depth == 1 ? 0x0202 : 0x83e0);
        sprite(t, 0x65000000, 404, 60 + depth * 6, 0, 0, clut, 1, 1);
        sprite(t, 0x65000000, 405, 60 + depth * 6, depth == 0 ? 64 : 32, 0, clut, 1, 1);
        sprite(t, 0x65000000, 406, 60 + depth * 6, 0, 0, clut, 1, 1);
        sprite(t, 0x65000000, 407, 60 + depth * 6, 0, depth < 2 ? 64 : 32, clut, 1, 1);
        sprite(t, 0x65000000, 408, 60 + depth * 6, 0, 0, clut, 1, 1);
    }
    GP0(t, 0xe1000000, 0x01000000); pixel(t, 0, 0, 0x1111);
    sprite(t, 0x65000000, 420, 60, 0, 0, clut, 1, 1); pixel(t, 321, 300, 0x3e0);
    sprite(t, 0x65000000, 421, 60, 0, 0, clut, 1, 1);
    GP0(t, 0xe1000080); pixel(t, 0, 0, 0x0101); GP0(t, 0x01000000);
    sprite(t, 0x65000000, 422, 60, 0, 0, clut, 1, 1); pixel(t, 321, 300, 0x7c00);
    GP0(t, 0xe1000000); sprite(t, 0x65000000, 423, 60, 0, 0, clut, 1, 1);
    GP0(t, 0xe1000100, 0xe2000000 | 1 | 2 << 5 | 1 << 10 | 2 << 15, 0x01000000);
    pixel(t, 8, 16, 0x4321); sprite(t, 0x65000000, 400, 90, 0, 0, 0, 1, 1);
    GP0(t, 0xe2000000, 0xe1003100, 0x01000000);
    const uint16_t flipped[] = {0x1f, 0x3e0}; upload(t, 5, 5, flipped, 2); upload(t, 5, 4, flipped, 2);
    sprite(t, 0x65000000, 400, 94, 5, 5, 0, 2, 2);
    GP0(t, 0xe1000100, 0x01000000);
    for (unsigned i = 0; i < 64; i++) source[i] = (uint16_t)(i + 1);
    upload(t, 0, 0, source, 64);
    GP0(t, 0x25000000, V(400, 100), UV(0, 0, 0), V(404, 100), UV(4, 0, 0x100), V(400, 104), UV(0, 4, 0));
    GP0(t, 0x2d000000, V(410, 100), UV(0, 0, 0), V(414, 100), UV(4, 0, 0x100), V(410, 104), UV(0, 4, 0), V(414, 104), UV(4, 4, 0));
    GP0(t, 0xe1000300, 0x34808080, V(430, 100), UV(0, 0, 0), 0, V(434, 100), UV(0, 0, 0x100), 0x808080, V(430, 104), UV(0, 0, 0));
    for (unsigned mode = 0; mode < 4; mode++) {
        GP0(t, 0xe1000100 | mode << 5, 0x01000000); pixel(t, 0, 0, 0x800a); pixel(t, 1, 0, 10);
        sprite(t, 0x67808080, 400 + mode * 4, 110, 0, 0, 0, 2, 1);
    }
    GP0(t, 0xe1000100, 0x01000000); upload(t, 0, 0, source, 64);
    sprite(t, 0x65000000, 1, 0, 0, 0, 0, 16, 1);   // cached blocks meet their own writes, in scan order
    GP0(t, 0x01000000); sprite(t, 0x65000000, 0, 1, 0, 0, 0, 16, 3);
    frame(t);
}

static void displays(fixture *t) {
    const uint16_t rgb[] = {0x2211, 0x4433, 0x6655, 0x8877}; upload(t, 1022, 511, rgb, 4);
    gpu_gp1(t->m, 0x05000000 | 1022 | 511 << 10); gpu_gp1(t->m, 0x08000011); frame(t);
    gpu_gp1(t->m, 0x08000000); frame(t); gpu_gp1(t->m, 0x08000040); frame(t);
    gpu_gp1(t->m, 0x08000009); gpu_gp1(t->m, 0x07000000 | 16 | 304 << 10); frame(t);
    gpu_gp1(t->m, 0x08000027); frame(t); gpu_gp1(t->m, 0x03000001); frame(t);
    drawing(t);
    for (unsigned f = 0; f < 8; f++) { GP0(t, 0x02000000 | f << 5, V(0, 0), V(32, 32)); frame(t); }
}

static uint32_t next(uint32_t *seed) { *seed = *seed * 1664525u + 1013904223u; return *seed; }
static void gradients(fixture *t) {
    // Irregular triangles exercise signed 12-bit gradient truncation and rounding, not merely whole-number slopes. The seed is fixed; all pixels stay synthetic.
    uint32_t seed = 0x612abcde; uint16_t source[256]; unsigned clut = 40 | 384 << 6;
    drawing(t);
    for (unsigned y = 0; y < 16; y++) { for (unsigned x = 0; x < 256; x++) source[x] = (uint16_t)next(&seed); upload(t, 512, y, source, 256); }
    for (unsigned x = 0; x < 256; x++) source[x] = (uint16_t)next(&seed);
    upload(t, 640, 384, source, 256); GP0(t, 0x01000000, 0xe4000000 | 319 | 239 << 10);
    for (unsigned i = 0; i < 32; i++) {
        uint32_t page = 8 | (i % 3) << 7 | (i % 4) << 5 | (i & 1) << 9;
        GP0(t, 0xe1000000 | page, 0xe6000000 | i % 4);
        uint32_t xy[3], uv[3], col[3];
        for (int k = 0; k < 3; k++) {
            int x = (int)(next(&seed) % 352) - 16, y = (int)(next(&seed) % 272) - 16;
            unsigned u = next(&seed) % 16, v = next(&seed) % 16;
            xy[k] = V(x, y); uv[k] = UV(u, v, k == 0 ? clut : k == 1 ? page : 0); col[k] = next(&seed) & 0xffffff;
        }
        GP0(t, 0x34000000 | (i & 3) << 24 | col[0], xy[0], uv[0], col[1], xy[1], uv[1], col[2], xy[2], uv[2]);
    }
    frame(t);
}

static void sprites(fixture *t) {
    // Keep several complete sprites queued before synchronization: the small-sprite kernel must preserve inter-command VRAM/cache effects as well as per-pixel math.
    uint16_t palette[256], source[256]; unsigned clut = 48 | 300 << 6;
    drawing(t); for (unsigned i = 0; i < 256; i++) palette[i] = (uint16_t)(((i + 1) * 147 & 0x7fff) | (i & 1 ? 0x8000 : 0));
    upload(t, 768, 300, palette, 256);
    for (unsigned depth = 0; depth < 4; depth++) {
        GP0(t, 0xe6000000, 0x02404040, V(512, 0), V(256, 256));
        for (unsigned y = 0; y < 32; y++) {
            unsigned row = y < 16 ? y : y + 224;
            for (unsigned x = 0; x < 256; x++) source[x] = (uint16_t)(((x + 1) * (row + 7) * 73 & 0x7fff) | ((x + row) & 1 ? 0x8000 : 0));
            upload(t, 512, row, source, 256);
        }
        GP0(t, 0x01000000, 0xe3000000 | 20 | 350 << 10, 0xe4000000 | 340 | 380 << 10);
        for (unsigned i = 0; i < 16; i++) {
            uint32_t page = 8 | depth << 7 | (i & 3) << 5 | (i & 4 ? 0x1000 : 0) | (i & 8 ? 0x2000 : 0);
            BATCH(t, 0xe1000000 | page, 0xe6000000 | (i & 3), 0x64000000 | (i & 3) << 24 | ((i + 1) * 0x101010 & 0xffffff),
                  V(16 + i * 19, 362 + i % 3 * 8), UV(250 + i % 3, 250 - i % 2, clut), V(i + 1, 16 - i % 13));
        }
        equal(t); frame(t);
    }
    drawing(t); GP0(t, 0xe1000008, 0x01000000); pixel(t, 512, 0, 0x1111); pixel(t, 769, 300, 0x1f);
    BATCH(t, 0x65000000, V(40, 400), UV(0, 0, clut), V(8, 8), 0x65000000, V(44, 404), UV(0, 0, clut), V(8, 8));
    BATCH(t, 0xa0000000, V(512, 0), V(1, 1), 0x2222, 0xa0000000, V(769, 300), V(1, 1), 0x3e0);
    BATCH(t, 0x65000000, V(60, 400), UV(0, 0, clut), V(8, 8), 0x65000000, V(64, 404), UV(0, 0, clut), V(8, 8));
    // An 8-bit draw promotes the stale 16-entry palette to 256 entries; the following 4-bit draw must keep the refreshed larger palette and cached texture words.
    BATCH(t, 0xe1000088, 0x65000000, V(80, 400), UV(0, 0, clut), V(8, 8), 0xe1000008, 0x65000000, V(84, 404), UV(0, 0, clut), V(8, 8));
    equal(t);
    GP0(t, 0xe1000108, 0x01000000);
    BATCH(t, 0x65000000, V(300, 420), UV(0, 0, 0), V(12, 12), 0x65000000, V(513, 0), UV(0, 0, 0), V(16, 1),
          0x65000000, V(304, 424), UV(0, 0, 0), V(12, 12));
    equal(t); frame(t);   // source/destination feedback between fast batches must use the ordered fallback, not a parallel snapshot
}

static void large_rectangles(int scale) {
    fixture t = create(scale); if (!t.r) { drop(&t); return; }
    uint16_t palette[256]; unsigned clut = 63 | 300 << 6;
    drawing(&t);
    for (unsigned i = 0; i < 256; i++) palette[i] = (uint16_t)((i * 173 & 0x7fff) | (i & 1 ? 0x8000 : 0));
    palette[1] = 0x801f; palette[2] = 0x83e0; upload(&t, 1008, 300, palette, 256);
    for (unsigned depth = 0; depth < 4; depth++) {
        // A page crossing VRAM's right edge gives each row/cache line distinct synthetic data, including transparent and semitransparent samples.
        GP0(&t, 0xe6000000); uint32_t seed = 0x71c12e03 + depth;
        gpu_gp0(t.m, 0xa0000000); gpu_gp0(t.m, V(960, 0)); gpu_gp0(t.m, V(256, 256));
        for (unsigned i = 0; i < 256 * 256; i += 2) {
            uint16_t a = i % 17 ? (uint16_t)next(&seed) : 0, b = i % 23 ? (uint16_t)next(&seed) : 0;
            gpu_gp0(t.m, a | (uint32_t)b << 16);
        }
        equal(&t); drawing(&t);
        for (unsigned flip = 0; flip < 4; flip++) {
            uint32_t page = 15 | depth << 7 | flip << 5 | (flip & 1 ? 0x1000 : 0) | (flip & 2 ? 0x2000 : 0);
            GP0(&t, 0xe1000000 | page, 0xe2000000 | 1 | 2 << 5 | 1 << 10 | 2 << 15,
                0xe3000000 | 320 | 200 << 10, 0xe4000000 | 389 | 259 << 10, 0xe6000000 | flip, 0x01000000);
            sprite(&t, 0x64607090 | flip << 24, 310, 190, 241, 235, clut, 95, 85);
        }
        drawing(&t); GP0(&t, 0xe1000000 | 15 | depth << 7, 0x01000000);
        sprite(&t, 0x65000000, 320, 200, 250, 250, clut, 300, 270);   // wrap both UV axes, repeatedly evicting direct-mapped cache slots
        uint16_t old = depth == 0 ? 0x1111 : depth == 1 ? 0x0101 : 0x801f;
        uint16_t fresh = depth == 0 ? 0x2222 : depth == 1 ? 0x0202 : 0x83e0;
        for (unsigned vertical = 0; vertical < 2; vertical++) {
            // The first visit must retain a stale line. A later alias evicts it; returning to the same UV after 256 pixels must fetch the changed VRAM word.
            pixel(&t, 960, 0, old); GP0(&t, 0x01000000);
            sprite(&t, 0x65000000, 700, 350, 0, 0, clut, 1, 1); pixel(&t, 960, 0, fresh);
            sprite(&t, 0x65000000, 320, 200, 0, 0, clut, vertical ? 17 : 300, vertical ? 270 : 65);
            CHECK(t.m->gpu.vram[200 * VRAM_W + 320] == 0x801f, "%dx depth %u starts with the stale texture line", scale, depth);
            CHECK(t.m->gpu.vram[(200 + (vertical ? 256 : 0)) * VRAM_W + 320 + (vertical ? 0 : 256)] == 0x83e0,
                  "%dx depth %u refetches the evicted texture line after %s UV wrap", scale, depth, vertical ? "vertical" : "horizontal");
        }
    }
    drawing(&t); GP0(&t, 0xe100000f); pixel(&t, 960, 0, 0x0101); pixel(&t, 1009, 300, 0x801f); GP0(&t, 0x01000000);
    sprite(&t, 0x65000000, 700, 350, 0, 0, clut, 1, 1); pixel(&t, 1009, 300, 0x83e0);
    sprite(&t, 0x65000000, 320, 200, 0, 0, clut, 33, 35);
    GP0(&t, 0xe100008f); sprite(&t, 0x65000000, 360, 200, 0, 0, clut, 33, 35);
    GP0(&t, 0xe100000f); sprite(&t, 0x65000000, 400, 200, 0, 0, clut, 33, 35);
    CHECK(t.m->gpu.vram[200 * VRAM_W + 320] == 0x801f && t.m->gpu.vram[200 * VRAM_W + 360] == 0x83e0 &&
          t.m->gpu.vram[200 * VRAM_W + 400] == 0x83e0, "%dx large rectangles retain stale CLUT data until 16-to-256-entry promotion", scale);
    gpu_gp1(t.m, 0x05000000 | 320 | 200 << 10);
    int cap_w = 640 * scale, cap_h = 576 * scale, w = 0, h = 0, sw = 0, sh = 0;
    uint32_t *out = calloc((size_t)cap_w * cap_h, sizeof *out), *ref = calloc(640 * 576, sizeof *ref);
    CHECK(out && ref, "%dx large-rectangle display buffers allocate", scale);
    if (out && ref) {
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx large rectangles verify native VRAM/cache", scale);
        ps1_display(t.m, ref, 640, 576, &sw, &sh); size_t mismatch = 0;
        if (w == sw * scale && h == sh * scale) for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
            mismatch += out[y * w + x] != ref[(y / scale) * sw + x / scale];
        CHECK(w == sw * scale && h == sh * scale && !mismatch, "%dx large sprites retain native texel footprints at every fine sample", scale);
    }
    free(out); free(ref);
    drawing(&t); GP0(&t, 0xe100010f, 0x01000000);
    sprite(&t, 0x65000000, 962, 1, 0, 0, 0, 33, 35);   // large texture feedback must keep scanline-ordered reads/writes
    GP0(&t, 0xe100000f, 0x01000000); sprite(&t, 0x65000000, 1008, 300, 0, 0, clut, 33, 35);   // destination overlaps the palette
    frame(&t); drop(&t);
}

static void textured_triangles(int scale) {
    fixture t = create(scale); if (!t.r) { drop(&t); return; } drawing(&t);
    uint16_t palette[256]; unsigned clut = 63 | 480 << 6;
    for (unsigned i = 0; i < 256; i++) palette[i] = (uint16_t)((i * 173 & 0x7fff) | (i & 1 ? 0x8000 : 0));
    palette[1] = 0x801f; palette[2] = 0x83e0; upload(&t, 1008, 480, palette, 256);
    for (unsigned depth = 0; depth < 4; depth++) {
        uint32_t seed = 0x71c12e03 + depth;
        gpu_gp0(t.m, 0xa0000000); gpu_gp0(t.m, V(960, 0)); gpu_gp0(t.m, V(256, 256));
        for (unsigned i = 0; i < 256 * 256; i += 2) {
            uint16_t a = i % 17 ? (uint16_t)next(&seed) : 0, b = i % 23 ? (uint16_t)next(&seed) : 0;
            gpu_gp0(t.m, a | (uint32_t)b << 16);
        }
        equal(&t);
        for (unsigned mode = 0; mode < 4; mode++) {
            uint32_t page = 15 | depth << 7 | mode << 5, xy[] = {V(310, 93), V(617, 109), V(389, 419)};
            uint32_t uv[] = {UV(3, 251, 0), UV(247, 5, 0), UV(19, 229, 0)}, color[] = {0x906040, 0x20f0a0, 0xf030d0};
            if (depth & 1) { uint32_t tmp = xy[1]; xy[1] = xy[2]; xy[2] = tmp; tmp = uv[1]; uv[1] = uv[2]; uv[2] = tmp; tmp = color[1]; color[1] = color[2]; color[2] = tmp; }
            drawing(&t);
            GP0(&t, 0x02404040, V(320, 100), V(320, 340), 0xe6000001, 0x60808080, V(320, 100), V(160, 340),
                0xe6000000 | mode, 0xe1000000 | page | (mode & 2 ? 512 : 0),
                0xe2000000 | (mode & 1 ? 1 | 2 << 5 | 1 << 10 | 2 << 15 : 0),
                0xe3000000 | 327 | 113 << 10, 0xe4000000 | 601 | 383 << 10, 0xe5000000 | 5 | 0x7fd << 11, 0x01000000);
            uint32_t cmd = (mode & 2 ? 0x36000000 : 0x26000000) | (mode & 1) << 24 | color[0];
            if (mode & 2) GP0(&t, cmd, xy[0], uv[0] | clut << 16, color[1], xy[1], uv[1] | page << 16, color[2], xy[2], uv[2]);
            else GP0(&t, cmd, xy[0], uv[0] | clut << 16, xy[1], uv[1] | page << 16, xy[2], uv[2]);
            CHECK(!(mode & 2) || t.m->gpu.vram[200 * VRAM_W + 400] == 0xc210, "%dx depth %u textured triangle preserves a protected pixel", scale, depth);
            CHECK(t.m->gpu.vram[100 * VRAM_W + 320] == 0xc210, "%dx depth %u textured triangle retains pixels outside its clip", scale, depth);
        }
        drawing(&t); GP0(&t, 0xe1000000 | 15 | depth << 7);
        for (unsigned holes_first = 0; holes_first < 2; holes_first++) {
            // A triangle's bounding box contains uncovered pixels. In the second shape, those holes precede the first cache eviction in raster order.
            // They must not terminate the stale prefix; a later covered alias must evict it before the next row/return to the original texture word.
            pixel(&t, 960, 0, depth == 0 ? 0x1111 : depth == 1 ? 0x0101 : 0x801f); GP0(&t, 0x01000000);
            sprite(&t, 0x65000000, 700, 450, 0, 0, clut, 1, 1); pixel(&t, 960, 0, depth == 0 ? 0x2222 : depth == 1 ? 0x0202 : 0x83e0);
            GP0(&t, 0x25000000, V(320, 100), UV(0, 0, clut), V(620, holes_first ? 400 : 100), UV(255, 0, 15 | depth << 7), V(320, 400), UV(0, 0, 0));
            CHECK(t.m->gpu.vram[(100 + holes_first) * VRAM_W + 320] == 0x801f, "%dx depth %u triangle keeps its stale prefix with %s holes", scale, depth, holes_first ? "leading" : "trailing");
            CHECK(t.m->gpu.vram[(holes_first ? 350 : 101) * VRAM_W + 320] == 0x83e0, "%dx depth %u triangle fetches fresh data after a covered eviction", scale, depth);
        }
    }
    drawing(&t); GP0(&t, 0xe100010f, 0x01000000);
    GP0(&t, 0x25000000, V(962, 1), UV(0, 0, 0), V(995, 1), UV(33, 0, 0x10f), V(962, 35), UV(0, 34, 0));
    GP0(&t, 0x01000000, 0x25000000, V(1008, 480), UV(0, 0, clut), V(1041, 480), UV(33, 0, 15), V(1008, 515), UV(0, 35, 0));
    GP0(&t, 0x01000000, 0x25000000, V(10, 10), UV(0, 0, clut ^ 1), V(20, 20), UV(255, 0, 15), V(30, 30), UV(0, 255, 0));
    GP0(&t, 0xe4000000 | 100 | 100 << 10, 0x25000000, V(320, 100), UV(0, 0, clut ^ 1), V(620, 100), UV(255, 0, 15), V(320, 400), UV(0, 255, 0));
    drawing(&t); GP0(&t, 0x02000000, V(0, 0), V(320, 240));
    uint16_t source[256]; for (unsigned i = 0; i < 256; i++) source[i] = i & 1 ? 0x3e0 : 0x1f;
    upload(&t, 512, 0, source, 256); pixel(&t, 768, 300, 0); pixel(&t, 769, 300, 0x801f); pixel(&t, 512, 0, 0x1111);
    GP0(&t, 0xe1000008, 0x01000000, 0x25000000, V(10, 10), UV(0, 0, 48 | 300 << 6), V(12, 11), UV(0, 0, 8), V(11, 11), UV(0, 0, 0));
    CHECK(!t.m->gpu.clut_valid, "%dx nondegenerate triangle without native samples does not load the palette", scale);
    int tags = 0; for (unsigned i = 0; i < 256; i++) tags += t.m->gpu.textag[i] != 0;
    CHECK(!tags, "%dx nondegenerate triangle without native samples does not load texture cache lines", scale);
    int cap_w = 640 * scale, cap_h = 576 * scale, w = 0, h = 0, sw = 0, sh = 0;
    uint32_t *out = calloc((size_t)cap_w * cap_h, sizeof *out), *ref = calloc(640 * 576, sizeof *ref);
    CHECK(out && ref, "%dx textured-triangle display buffers allocate", scale);
    if (out && ref) {
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx thin textured triangle verifies untouched native caches", scale);
        int colored = 0; for (int i = 0; i < w * h; i++) colored += (out[i] & 0xffffff) != 0;
        CHECK(colored == scale * (scale - 1) / 2, "%dx native-empty triangle still draws %d fine samples", scale, colored);
        GP0(&t, 0x02000000, V(0, 0), V(320, 240)); upload(&t, 512, 0, source, 256); GP0(&t, 0xe1000108, 0x01000000);
        sprite(&t, 0x65000000, 700, 450, 0, 0, 0, 1, 1); pixel(&t, 512, 0, 0x7c00);
        // Native sample records at y=0, x<256 previously occupied the same scratch words as cache events. Their stale values must survive until fine shading.
        GP0(&t, 0x25000000, V(0, 0), UV(0, 0, 0), V(200, 0), UV(200, 0, 0x108), V(0, 200), UV(0, 0, 0));
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx textured triangle verifies recorded native samples", scale);
        ps1_display(t.m, ref, 640, 576, &sw, &sh); size_t mismatch = 0;
        if (w == sw * scale && h == sh * scale) for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) mismatch += out[y * scale * w + x * scale] != ref[y * sw + x];
        CHECK(w == sw * scale && h == sh * scale && !mismatch && out[0] == 0xffff0000, "%dx textured triangle fine grid retains every native sample, including stale low-row texels", scale);
        CHECK(out[12 * scale] == 0xffff0000 && (scale == 1 || out[12 * scale + scale / 2] == 0xff00ff00), "%dx textured triangle interpolates genuine subpixel UVs", scale);
        GP0(&t, 0x02000000, V(0, 0), V(320, 240)); pixel(&t, 512, 0, 0x7fff); GP0(&t, 0x01000000);
        GP0(&t, 0x34000000, V(0, 0), UV(0, 0, 0), 0x000000ff, V(200, 0), UV(0, 0, 0x108), 0x0000ff00, V(0, 200), UV(0, 0, 0));
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx Gouraud textured triangle verifies native VRAM/cache", scale);
        CHECK(out[16 * scale] == 0xff210000 && (scale == 1 || out[16 * scale + scale / 2] == 0xff290000), "%dx textured Gouraud colors interpolate between native samples", scale);
    }
    free(out); free(ref); drop(&t);
}

static void large_triangles(int scale) {
    fixture t = create(scale); if (!t.r) { drop(&t); return; } drawing(&t);
    // The BIOS profile found two large untextured triangles spending 63.59 ms in the serial shader. Synthetic screen-sized pairs exercise that area and the
    // shared diagonal without depending on BIOS commands or game data; opposite windings must cover the same 640x480 rectangle exactly once.
    for (unsigned winding = 0; winding < 2; winding++) {
        BATCH(&t, 0x02000000, V(0, 0), V(640, 480));
        if (winding) BATCH(&t, 0x204060a0, V(0, 0), V(0, 480), V(640, 0), 0x204060a0, V(640, 480), V(640, 0), V(0, 480));
        else BATCH(&t, 0x204060a0, V(0, 0), V(640, 0), V(0, 480), 0x204060a0, V(640, 480), V(0, 480), V(640, 0));
        equal(&t); int colored = 0;
        for (unsigned i = 0; i < VRAM_W * VRAM_H; i++) colored += t.m->gpu.vram[i] != 0;
        CHECK(colored == 640 * 480, "%dx large flat pair winding %u covers the complete screen", scale, winding);
    }
    frame(&t);
    for (unsigned gouraud = 0; gouraud < 2; gouraud++) for (unsigned mode = 0; mode < 4; mode++)
        for (unsigned mask = 0; mask < 4; mask++) for (unsigned dither = 0; dither < 2; dither++) {
            // Half the destination is protected, half writable. Signed coordinates, draw offsets and clipping make each gradient cross nontrivial pixel boundaries.
            BATCH(&t, 0xe5000000, 0xe3000000, 0xe4000000 | 319 | 239 << 10, 0xe6000000, 0x02404040, V(0, 0), V(320, 240),
                  0xe6000001, 0x60808080, V(0, 0), V(160, 240), 0xe6000000 | mask,
                  0xe1000000 | mode << 5 | dither << 9, 0xe3000000 | 11 | 7 << 10, 0xe4000000 | 309 | 227 << 10,
                  0xe5000000 | 5 | 0x7fd << 11);
            uint32_t b = V(337, 13), c = V(73, 269), cb = 0x00e04090, cc = 0x0020d060;
            if (mask & 1) { uint32_t tmp = b; b = c; c = tmp; tmp = cb; cb = cc; cc = tmp; }
            if (gouraud) BATCH(&t, 0x320000ff, V(-37, -29), cb, b, cc, c);
            else BATCH(&t, 0x22486098, V(-37, -29), b, c);
            equal(&t);
            CHECK(!(mask & 2) || t.m->gpu.vram[32 * VRAM_W + 32] == 0xc210,
                  "%dx %s triangle mode %u mask %u dither %u preserves protected pixels", scale, gouraud ? "Gouraud" : "flat", mode, mask, dither);
            CHECK(t.m->gpu.vram[0] == 0xc210 && t.m->gpu.vram[239 * VRAM_W + 319] == 0x2108,
                  "%dx %s triangle mode %u mask %u dither %u retains clipped pixels", scale, gouraud ? "Gouraud" : "flat", mode, mask, dither);
        }
    drawing(&t);
    uint16_t *before = malloc(sizeof t.m->gpu.vram);
    CHECK(before != NULL, "%dx rejected-triangle snapshot allocates", scale);
    if (before) {
        memcpy(before, t.m->gpu.vram, sizeof t.m->gpu.vram);
        BATCH(&t, 0x20ffffff, V(10, 10), V(300, 300), V(490, 490),
              0x30ffffff, V(10, 10), 0x000000ff, V(10, 10), 0x0000ff00, V(400, 400),
              0x20ffffff, V(-500, 10), V(600, 10), V(0, 300),
              0x300000ff, V(10, -250), 0x0000ff00, V(300, 300), 0x00ff0000, V(10, 400),
              0xe4000000 | 100 | 100 << 10, 0x20ffffff, V(500, 300), V(800, 300), V(500, 500));
        equal(&t);
        CHECK(!memcmp(before, t.m->gpu.vram, sizeof t.m->gpu.vram), "%dx degenerate, oversized and fully clipped triangles write no pixels", scale);
        free(before);
    }
    drawing(&t); GP0(&t, 0x02000000, V(0, 0), V(640, 480), 0x2000c0ff, V(20, 20), V(221, 20), V(20, 221));
    int cap_w = 640 * scale, cap_h = 576 * scale, w = 0, h = 0;
    uint32_t *out = calloc((size_t)cap_w * cap_h, sizeof *out);
    CHECK(out != NULL, "%dx large-triangle display buffer allocates", scale);
    if (out) {
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx large flat triangle verifies native VRAM/cache", scale);
        int colored = 0, edge = 201 * scale, expected = edge * (edge + 1) / 2;
        for (int i = 0; i < w * h; i++) colored += (out[i] & 0xffffff) != 0;
        CHECK(w == 320 * scale && h == 240 * scale && colored == expected, "%dx large diagonal contains %d fine samples, expected %d", scale, colored, expected);
        CHECK(scale == 1 || colored != (201 * 202 / 2) * scale * scale, "%dx large triangle retains genuine fine geometry", scale);
        GP0(&t, 0x02000000, V(0, 0), V(640, 480), 0x30000000, V(20, 20), 0x000000ff, V(220, 20), 0x0000ff00, V(20, 220));
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx large Gouraud triangle verifies native VRAM/cache", scale);
        if (scale > 1) CHECK(out[20 * scale * w + 32 * scale] != out[20 * scale * w + 32 * scale + scale / 2],
                             "%dx Gouraud color interpolates between native samples", scale);
        free(out);
    }
    frame(&t); drop(&t);
}

static void hires(int scale) {
    fixture t = create(scale); if (!t.r) { drop(&t); return; } drawing(&t);
    GP0(&t, 0x2000c0ff, V(10, 10), V(15, 10), V(10, 15));
    int cap_w = 640 * scale, cap_h = 576 * scale, w = 0, h = 0; uint32_t *out = calloc((size_t)cap_w * cap_h, sizeof *out);
    CHECK(out != NULL, "high-resolution frame buffer allocates");
    if (out) {
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx geometry frame matches native reference", scale);
        int pixels = 0; for (int i = 0; i < w * h; i++) pixels += (out[i] & 0xffffff) != 0;
        int edge = 5 * scale, expected = edge * (edge + 1) / 2;
        CHECK(w == 320 * scale && h == 240 * scale, "%dx has the requested actual render dimensions", scale);
        CHECK(pixels == expected && pixels != 15 * scale * scale, "%dx triangle has %d subpixels, expected %d; nearest-neighbor would have %d", scale, pixels, expected, 15 * scale * scale);
        uint32_t fine[16];
        for (int y = 0; y < scale; y++) for (int x = 0; x < scale; x++) fine[y * scale + x] = out[(10 * scale + y) * w + 14 * scale + x];
        GP0(&t, 0xe1000100, 0x01000000); sprite(&t, 0x65000000, 14, 10, 0, 0, 0, 1, 1);
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx transparent feedback keeps the fine triangle edge", scale);
        for (int y = 0; y < scale; y++) for (int x = 0; x < scale; x++)
            CHECK(out[(10 * scale + y) * w + 14 * scale + x] == fine[y * scale + x], "%dx transparent feedback preserves subpixel (%d,%d)", scale, x, y);
        pixel(&t, 0, 0, 0x7fff); GP0(&t, 0xe6000001, 0x2000c0ff, V(10, 10), V(15, 10), V(10, 15), 0xe6000002, 0x01000000);
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx masked triangle frame verifies", scale);
        for (int y = 0; y < scale; y++) for (int x = 0; x < scale; x++) fine[y * scale + x] = out[(10 * scale + y) * w + 14 * scale + x];
        sprite(&t, 0x65000000, 14, 10, 0, 0, 0, 1, 1);
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx masked feedback keeps the fine triangle edge", scale);
        for (int y = 0; y < scale; y++) for (int x = 0; x < scale; x++)
            CHECK(out[(10 * scale + y) * w + 14 * scale + x] == fine[y * scale + x], "%dx masked feedback preserves subpixel (%d,%d)", scale, x, y);
        GP0(&t, 0xe1000108, 0x01000000); sprite(&t, 0x65000000, 10, 10, 0, 0, 0, 33, 35);
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx large transparent nonfeedback rectangle verifies", scale);
        for (int y = 0; y < scale; y++) for (int x = 0; x < scale; x++)
            CHECK(out[(10 * scale + y) * w + 14 * scale + x] == fine[y * scale + x], "%dx large transparent rectangle preserves fine sample (%d,%d)", scale, x, y);
        GP0(&t, 0xe6000000, 0x60ffffff, V(512, 0), V(33, 35), 0xe6000002, 0x01000000);
        sprite(&t, 0x65000000, 10, 10, 0, 0, 0, 33, 35);
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx large masked nonfeedback rectangle verifies", scale);
        int protected = 0, updated = 0;
        for (int y = 0; y < scale; y++) for (int x = 0; x < scale; x++) {
            uint32_t old = fine[y * scale + x]; int keep = (old & 0xffffff) != 0; protected += keep; updated += !keep;
            CHECK(out[(10 * scale + y) * w + 14 * scale + x] == (keep ? old : 0xffffffff), "%dx large masked rectangle %s fine sample (%d,%d)", scale, keep ? "preserves protected" : "updates unprotected", x, y);
        }
        CHECK(protected && updated, "%dx masked rectangle tests both protected and unprotected children of one native pixel", scale);
        GP0(&t, 0xe6000000); pixel(&t, 768, 0, 0); GP0(&t, 0xe6000002, 0xe100010c, 0x01000000);
        GP0(&t, 0x25000000, V(10, 10), UV(0, 0, 0), V(43, 10), UV(0, 0, 0x10c), V(10, 45), UV(0, 0, 0));
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx transparent independent triangle verifies", scale);
        for (int y = 0; y < scale; y++) for (int x = 0; x < scale; x++) {
            uint32_t old = fine[y * scale + x];
            CHECK(out[(10 * scale + y) * w + 14 * scale + x] == ((old & 0xffffff) ? old : 0xffffffff), "%dx transparent triangle preserves fine sample (%d,%d)", scale, x, y);
        }
        GP0(&t, 0xe6000000); pixel(&t, 768, 0, 0x7c00); GP0(&t, 0xe6000002, 0x01000000);
        GP0(&t, 0x25000000, V(10, 10), UV(0, 0, 0), V(43, 10), UV(0, 0, 0x10c), V(10, 45), UV(0, 0, 0));
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx masked independent triangle verifies", scale);
        for (int y = 0; y < scale; y++) for (int x = 0; x < scale; x++) {
            uint32_t old = fine[y * scale + x];
            CHECK(out[(10 * scale + y) * w + 14 * scale + x] == ((old & 0xffffff) ? old : 0xff0000ff), "%dx masked triangle preserves protected and updates unprotected fine sample (%d,%d)", scale, x, y);
        }
        GP0(&t, 0xe6000000);
        pixel(&t, 767, 255, 0x1f); pixel(&t, 512, 255, 0x3e0); pixel(&t, 767, 0, 0x7c00); pixel(&t, 512, 0, 0x7fff);
        GP0(&t, 0x01000000);
        for (unsigned i = 0; i < 4; i++) BATCH(&t, 0xe1000108 | (i & 1 ? 0x1000 : 0) | (i & 2 ? 0x2000 : 0),
            0x65000000, V(20 + i * 4, 20), UV(255, i & 2 ? 0 : 255, 0), V(2, 2));
        equal(&t);
        CHECK(!metal_renderer_frame(t.r, out, cap_w, cap_h, &w, &h, 1), "%dx texture frame verifies native VRAM/cache", scale);
        const uint32_t rgb[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
        for (unsigned i = 0; i < 4; i++) for (int y = 0; y < 2 * scale; y++) for (int x = 0; x < 2 * scale; x++) {
            int sx = x / scale ^ (i & 1), sy = y / scale ^ !!(i & 2);
            CHECK(out[(20 * scale + y) * w + (20 + i * 4) * scale + x] == rgb[sy * 2 + sx], "%dx batched sprite flip %u samples subpixel (%d,%d)", scale, i, x, y);
        }
        free(out);
    }
    frame(&t); drop(&t);
}

int main(void) { @autoreleasepool {
    if (!MTLCreateSystemDefaultDevice()) { fputs("[RED] metal tests: no Metal device; GPU validation did not run\n", stderr); return 77; }
    fixture a = create(1), b = create(1);
    if (a.r && b.r) {
        primitives(&a); transfers(&a); textures(&a); displays(&a); gradients(&a); sprites(&a);
        drawing(&b); GP0(&b, 0x6800ff00, V(10, 10));
        const uint16_t *av = metal_renderer_vram(a.r), *bv = metal_renderer_vram(b.r);
        CHECK(av && bv && av[10 * VRAM_W + 10] != bv[10 * VRAM_W + 10], "two renderer instances retain independent VRAM");
        gpu_reset(a.m); equal(&a); equal(&b);
        metal_renderer_free(a.r); a.r = NULL; gpu_gp0(a.m, 0x00000000); equal(&b);
        CHECK(!a.m->on_render && !a.m->render_ctx, "freeing a renderer detaches its machine callback");
    }
    drop(&a); drop(&b); large_rectangles(1); large_rectangles(2); large_rectangles(4);
    large_triangles(1); large_triangles(2); large_triangles(4); textured_triangles(1); textured_triangles(2); textured_triangles(4); hires(2); hires(4);
    printf("metal: %d checks, %d failed\n", checks, fails); return fails != 0;
} }
