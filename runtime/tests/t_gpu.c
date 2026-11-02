// The GPU (gpu_sw.c; psx-spx, Graphics Processing Unit): GPUSTAT after reset, a fill with its rounding, polygons without their right and bottom edges, gouraud colour at
// a vertex, the drawing area and offset, lines with both ends, a polyline to its terminator, semi-transparency, the mask bit, VRAM transfers both ways with an odd pixel
// count, GP1(10h) readbacks, the display's mode bits and picture, and the vertical blank at the end of the display range
#include "ps1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static ps1 *m;
#define V(x, y) ((uint32_t)(y) << 16 | (uint32_t)(x))
#define GP0(...) do { const uint32_t w_[] = {__VA_ARGS__}; for (unsigned i_ = 0; i_ < sizeof w_ / sizeof *w_; i_++) gpu_gp0(m, w_[i_]); } while (0)
static uint16_t px(int x, int y) { return m->gpu.vram[y * VRAM_W + x]; }
static int drawn(int x0, int y0, int x1, int y1) { int n = 0; for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) n += px(x, y) != 0; return n; }

static void drawing_reports(void) {
    ps1 *saved = m;
    const struct { unsigned n; uint32_t w[4]; } commands[] = {
        {3, {0x020000ff, 0, V(1, 1)}}, {4, {0x200000ff, 0, V(1, 0), V(0, 1)}},
        {3, {0x400000ff, 0, V(1, 1)}}, {4, {0x480000ff, 0, V(1, 1), 0x50005000}},
        {4, {0x640000ff, 0, 0, V(1, 1)}}, {4, {0x80000000, 0, V(1, 1), V(1, 1)}},
        {3, {0xa0000000, 0, V(1, 1)}}, {3, {0xc0000000, 0, V(1, 1)}}
    };
    for (unsigned i = 0; i < sizeof commands / sizeof *commands; i++) for (int dma = 0; dma < 2; dma++) {
        m = ps1_new(); CHECK(m, "fresh machine for each drawing report"); if (!m) { m = saved; return; }
        if (dma) {
            for (unsigned j = 0; j < commands[i].n; j++) mem_write32(m, 0x10000 + j * 4, commands[i].w[j]);
            dma_write(m, 0x70, m->dpcr | 0x800); dma_write(m, 0x20, 0x10000); dma_write(m, 0x24, 0x10000 | commands[i].n);
            dma_write(m, 0x28, 0x01000201);
        } else {
            for (unsigned j = 0; j < commands[i].n; j++) gpu_gp0(m, commands[i].w[j]);
        }
        CHECK(m->red & RED_GPU_TIMING, "each unmodelled primitive/transfer reports drawing time through CPU GP0 and block DMA");
        CHECK(!(m->red & RED_DMA_LIST) && !m->gpu.draw_until, "the drawing gap needs no linked list and does not invent drawing occupancy");
        ps1_free(m);
    }
    m = ps1_new(); CHECK(m, "fresh machine for measured drawing"); if (!m) { m = saved; return; }
    GP0(0, 0xe1000000, 0x600000ff, 0, V(1, 1), 0x680000ff, 0, 0x700000ff, 0, 0x780000ff, 0);
    CHECK(!(m->red & RED_GPU_TIMING) && m->gpu.draw_until, "settings, no-ops and measured untextured rectangles do not report a drawing gap");
    ps1_free(m); m = saved;
}

int main(void) {
    static uint32_t pic[640 * 480];
    m = ps1_new();
    if (!m) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }
    drawing_reports();
    CHECK(gpu_stat(m) == 0x14802000, "GPUSTAT after reset is 14802000h");
    GP0(0xe1003fff);
    CHECK((gpu_stat(m) & 0x87ff) == 0x7ff, "GP0(E1h) bit 11 is ignored until GP1(09h) allows it (ps1-tests gpu/gp0-e1)");
    gpu_gp1(m, 0x09000001); GP0(0xe1000800);
    CHECK((gpu_stat(m) & 0x87ff) == 0x8000, "after GP1(09h), GP0(E1h) bit 11 shows in GPUSTAT.15");
    gpu_gp1(m, 0x09000000); GP0(0xe1000000);

    GP0(0x02ff0000, V(0x13, 2), V(0x11, 3));
    CHECK(px(16, 2) == 0x7c00 && px(47, 4) == 0x7c00 && drawn(0, 0, 63, 8) == 32 * 3, "fill: x rounded down and width up to 16 pixels, colour to 15 bits");

    GP0(0xe3000000, 0xe4000000 | 1023 | 511 << 10, 0xe5000000);
    GP0(0x28ffffff, V(100, 100), V(104, 100), V(100, 104), V(104, 104));
    CHECK(drawn(96, 96, 108, 108) == 16 && px(100, 100) == 0x7fff && px(103, 103) == 0x7fff, "a 4x4 quad: its right and bottom edges are left out");
    GP0(0x20ffffff, V(120, 100), V(124, 100), V(120, 104));
    CHECK(px(120, 100) && px(123, 100) && !px(124, 100) && !px(120, 104) && drawn(116, 96, 128, 108) == 10, "a triangle: the same rule on its slanted edge");
    GP0(0x300000ff, V(0, 300), 0x0000ff00, V(40, 300), 0x00ff0000, V(0, 340));
    CHECK(px(0, 300) == 0x001f, "gouraud: the first vertex's colour at the first vertex");

    GP0(0xe3000000 | 200 | 200 << 10, 0xe4000000 | 203 | 201 << 10, 0xe5000000 | 10 | 20 << 11);
    GP0(0x60ffffff, V(185, 175), V(20, 20));
    CHECK(drawn(190, 190, 220, 220) == 8 && px(200, 200) && px(203, 201), "a rectangle moved by the drawing offset and clipped to the drawing area");
    GP0(0xe3000000, 0xe4000000 | 1023 | 511 << 10, 0xe5000000);

    GP0(0x4000ff00, V(300, 10), V(303, 10));
    CHECK(drawn(296, 8, 308, 12) == 4 && px(303, 10) == 0x03e0, "a line includes both ends");
    GP0(0x480000ff, V(300, 20), V(302, 20), V(302, 22), 0x55555555, 0x02ff0000, V(320, 30), V(16, 1));
    CHECK(drawn(296, 18, 306, 26) == 5 && px(302, 22) == 0x001f && px(320, 30) == 0x7c00, "a polyline to its terminator, then the next command");

    GP0(0x02404040, V(400, 0), V(16, 3), 0xe1000020, 0x6a505050, V(400, 0));
    CHECK(px(400, 0) == (18 | 18 << 5 | 18 << 10), "semi-transparency B+F: 8 + 10");
    GP0(0xe1000040, 0x6a505050, V(401, 0), 0xe1000060, 0x6a505050, V(402, 0), 0xe1000000);
    CHECK(px(401, 0) == 0 && px(402, 0) == (10 | 10 << 5 | 10 << 10), "semi-transparency B-F clamps at 0, B+F/4: 8 + 2");

    GP0(0xe6000001, 0x68ffffff, V(500, 0), 0xe6000002, 0x680000ff, V(500, 0), 0xe6000000);
    CHECK(px(500, 0) == 0xffff, "the mask bit: set while drawing, then the pixel is kept from drawing");

    GP0(0xa0000000, V(600, 300), V(3, 1), 0x22221111, 0x00003333);
    CHECK(px(600, 300) == 0x1111 && px(601, 300) == 0x2222 && px(602, 300) == 0x3333 && px(603, 300) == 0, "CPU to VRAM: an odd count ends on a padding halfword");
    GP0(0xc0000000, V(600, 300), V(3, 1));
    uint32_t ready = gpu_stat(m) >> 27 & 1, a = gpu_read(m), b = gpu_read(m);
    CHECK(ready && a == 0x22221111 && b == 0x3333 && !(gpu_stat(m) >> 27 & 1), "VRAM to CPU: two pixels a word, GPUSTAT.27 while there are some to read");

    GP0(0xe3000000 | 5 | 6 << 10);
    gpu_gp1(m, 0x10000003); uint32_t area = gpu_read(m);
    gpu_gp1(m, 0x10000007); uint32_t version = gpu_read(m);
    gpu_gp1(m, 0x10000000);
    CHECK(area == (5 | 6 << 10) && version == 2 && gpu_read(m) == 2, "GP1(10h): drawing area, version 2, and index 0 leaves the latch alone");
    GP0(0xe3000000);

    gpu_gp1(m, 0x08000027); gpu_gp1(m, 0x03000000);
    int w, h;
    ps1_display(m, pic, 640, 480, &w, &h);
    CHECK((gpu_stat(m) >> 16 & 0x7f) == (3 << 1 | 1 << 3 | 1 << 6) && !(gpu_stat(m) >> 23 & 1), "GPUSTAT: 640 pixels, 480 lines, interlaced, display on");
    CHECK(w == 640 && h == 480 && pic[100 * 640 + 100] == 0xffffffff && pic[0] == 0xff000000, "the picture: 640x480 from VRAM's top left, BGRA");
    gpu_gp1(m, 0x08000001);
    ps1_display(m, pic, 640, 480, &w, &h);
    CHECK(w == 320 && h == 240, "320x240");
    gpu_gp1(m, 0x03000001);
    ps1_display(m, pic, 640, 480, &w, &h);
    CHECK(pic[100 * 320 + 100] == 0xff000000, "display off: black");

    ps1_free(m); m = ps1_new();
    while (!(m->i_stat & 1u << IRQ_VBLANK)) { m->cycles = m->next_event; sched_run(m); }
    CHECK(m->gpu.line == m->gpu.vy2 && m->frame_done && m->cycles / m->gpu.vy2 == 2152, "the vertical blank IRQ at the display range's end, after 2152.85 cycles a line");
    CHECK(!(gpu_stat(m) >> 31), "GPUSTAT.31 is 0 in the vertical blank");

    printf("gpu: %d checks, %d failed\n", checks, fails);
    ps1_free(m);
    return fails != 0;
}
