// The GPU (psx-spx, Graphics Processing Unit): GP0 commands draw into the 1 MB VRAM and move rectangles in and out of it, GP1 commands set up the display, and GPUSTAT
// reports both. Drawing is done in software as each command completes: untextured polygons (flat or gouraud), lines and polylines, rectangles, fills and copies, with
// semi-transparency, dithering and the mask bit. The video timing (GPU Timings) drives the scanline events: hblank for timer 1, the vertical blank IRQ, the end of a frame.
// Not modelled yet (phase 2): textures, and the lines skipped when drawing to the displayed field of an interlaced picture
#include "ps1.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static int sext11(uint32_t v) { return (int32_t)(v << 21) >> 21; }
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// 24-bit colour to the 15-bit framebuffer format, dithered by a 4x4 pattern of offsets when asked (psx-spx, 24bit RGB to 15bit RGB Dithering)
static uint16_t rgb15(int x, int y, int r, int gr, int b, int dither) {
    if (dither) {
        static const int8_t offset[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
        int o = offset[y & 3][x & 3];
        r = clamp8(r + o); gr = clamp8(gr + o); b = clamp8(b + o);
    }
    return (uint16_t)(r >> 3 | (gr >> 3) << 5 | (b >> 3) << 10);
}

static uint16_t blend(uint16_t back, uint16_t front, uint32_t mode) {   // per 5-bit channel, saturated (psx-spx, Semi-transparency)
    uint16_t out = 0;
    for (int s = 0; s < 15; s += 5) {
        int b = back >> s & 31, f = front >> s & 31, v = mode == 0 ? (b + f) >> 1 : mode == 1 ? b + f : mode == 2 ? b - f : b + (f >> 2);
        out |= (uint16_t)((v < 0 ? 0 : v > 31 ? 31 : v) << s);
    }
    return out;
}

static void plot(ps1_gpu *g, int x, int y, uint16_t c, int semi) {   // the mask bit setting, GP0(E6h), applies to every drawing command but fills
    uint16_t *p = &g->vram[(y & 511) * VRAM_W + (x & 1023)];
    if (g->mask & 2 && *p & 0x8000) return;
    if (semi) c = blend(*p, c, g->texpage >> 5 & 3);
    *p = c | (uint16_t)((g->mask & 1) << 15);
}

static ps1_vtx vtx(const ps1_gpu *g, uint32_t pos, uint32_t col) {   // 11-bit signed coordinates, moved by the drawing offset
    return (ps1_vtx){sext11(pos) + g->ox, sext11(pos >> 16) + g->oy, (int)(col & 0xff), (int)(col >> 8 & 0xff), (int)(col >> 16 & 0xff)};
}

static int64_t edge(const ps1_vtx *p, const ps1_vtx *q, int x, int y) { return (int64_t)(q->x - p->x) * (y - p->y) - (int64_t)(q->y - p->y) * (x - p->x); }
static int owns(const ps1_vtx *p, const ps1_vtx *q) { return q->y < p->y || (q->y == p->y && q->x > p->x); }   // top and left edges keep the pixels on them

// Polygons leave out their right and bottom edges, are not drawn when wider than 1023 or taller than 511, and are dithered only when shaded (psx-spx, Vertex; Notes)
static void triangle(ps1_gpu *g, ps1_vtx a, ps1_vtx b, ps1_vtx c, int gouraud, int semi) {
    int x0 = MIN(MIN(a.x, b.x), c.x), x1 = MAX(MAX(a.x, b.x), c.x), y0 = MIN(MIN(a.y, b.y), c.y), y1 = MAX(MAX(a.y, b.y), c.y);
    if (x1 - x0 > 1023 || y1 - y0 > 511) return;
    int64_t area = edge(&a, &b, c.x, c.y);
    if (!area) return;
    if (area < 0) { ps1_vtx t = b; b = c; c = t; area = -area; }
    x0 = MAX(x0, g->ax1); x1 = MIN(x1, g->ax2); y0 = MAX(y0, g->ay1); y1 = MIN(y1, g->ay2);
    int dither = gouraud && g->texpage >> 9 & 1;
    uint16_t flat = rgb15(0, 0, a.r, a.g, a.b, 0);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int64_t wa = edge(&b, &c, x, y), wb = edge(&c, &a, x, y), wc = edge(&a, &b, x, y);
            if (wa < 0 || wb < 0 || wc < 0 || (!wa && !owns(&b, &c)) || (!wb && !owns(&c, &a)) || (!wc && !owns(&a, &b))) continue;
            uint16_t col = flat;
            if (gouraud)
                col = rgb15(x, y, (int)((a.r * wa + b.r * wb + c.r * wc) / area), (int)((a.g * wa + b.g * wb + c.g * wc) / area),
                            (int)((a.b * wa + b.b * wb + c.b * wc) / area), dither);
            plot(g, x, y, col, semi);
        }
}

static void dot(ps1_gpu *g, int x, int y, int r, int gr, int b, int semi, int dither) {
    if (x >= g->ax1 && x <= g->ax2 && y >= g->ay1 && y <= g->ay2) plot(g, x, y, rgb15(x, y, r, gr, b, dither), semi);
}

// Lines include both ends and are always dithered when dithering is on; ends at one place draw a 1x1 rectangle in the first colour (psx-spx, GPU Render Line Commands)
static void line(ps1_gpu *g, const ps1_vtx *a, const ps1_vtx *b, int gouraud, int semi) {
    int dx = b->x - a->x, dy = b->y - a->y, n = MAX(abs(dx), abs(dy));
    if (abs(dx) > 1023 || abs(dy) > 511) return;
    if (!n) { dot(g, a->x, a->y, a->r, a->g, a->b, semi, 0); return; }
    int dither = g->texpage >> 9 & 1;
    int64_t x = (int64_t)a->x * 65536 + 0x8000, y = (int64_t)a->y * 65536 + 0x8000, sx = (int64_t)dx * 65536 / n, sy = (int64_t)dy * 65536 / n;
    for (int i = 0; i <= n; i++, x += sx, y += sy) {
        int r = a->r, gr = a->g, bl = a->b;
        if (gouraud) { r += (b->r - a->r) * i / n; gr += (b->g - a->g) * i / n; bl += (b->b - a->b) * i / n; }
        dot(g, (int)(x >> 16), (int)(y >> 16), r, gr, bl, semi, dither);
    }
}

static void polygon(ps1 *m, uint32_t c) {
    ps1_gpu *g = &m->gpu;
    const uint32_t *w = g->buf;
    int gouraud = c >> 4 & 1, n = c & 8 ? 4 : 3, tex = c >> 2 & 1, p = 1;
    ps1_vtx v[4];
    for (int k = 0; k < n; k++) {
        uint32_t col = gouraud && k ? w[p++] : w[0];
        v[k] = vtx(g, w[p++], col);
        if (tex && k == 1) g->texpage = (g->texpage & ~0x9ffu) | (w[p] >> 16 & 0x9ff);   // the page attribute sets GP0(E1h) bits 0-8 and 11
        p += tex;
    }
    if (tex) { RED_ONCE(m, RED_TEXTURE, "gpu: textured polygons are not drawn yet (phase 2)"); return; }
    triangle(g, v[0], v[1], v[2], gouraud, c >> 1 & 1);
    if (n == 4) triangle(g, v[1], v[2], v[3], gouraud, c >> 1 & 1);   // a quad is the triangles 1-2-3 and 2-3-4
}

static void rectangle(ps1 *m, uint32_t c) {
    static const int side[4] = {0, 1, 8, 16};
    ps1_gpu *g = &m->gpu;
    int tex = c >> 2 & 1, w = side[c >> 3 & 3], h = w;
    if (tex) { RED_ONCE(m, RED_TEXTURE, "gpu: textured rectangles are not drawn yet (phase 2)"); return; }
    if (!w) { w = g->buf[2 + tex] & 0x3ff; h = g->buf[2 + tex] >> 16 & 0x1ff; }
    ps1_vtx v = vtx(g, g->buf[1], g->buf[0]);
    uint16_t col = rgb15(0, 0, v.r, v.g, v.b, 0);
    for (int y = MAX(v.y, g->ay1); y <= MIN(v.y + h - 1, g->ay2); y++)
        for (int x = MAX(v.x, g->ax1); x <= MIN(v.x + w - 1, g->ax2); x++) plot(g, x, y, col, c >> 1 & 1);
}

static void poly_word(ps1_gpu *g, uint32_t w) {   // a polyline's words after its command, up to the terminator, checked on a vertex's first word once two have come
    int first = !g->pgour || !g->phave;
    if (first && g->pnv >= 2 && (w & 0xf000f000) == 0x50005000) { g->poly = 0; return; }
    if (!first) { ps1_vtx v = vtx(g, w, g->pcol); if (g->pnv) line(g, &g->plast, &v, 1, g->psemi); g->plast = v; g->phave = 0; g->pnv += g->pnv < 2; return; }
    if (g->pgour) { g->pcol = w; g->phave = 1; return; }
    ps1_vtx v = vtx(g, w, g->pcol);
    if (g->pnv) line(g, &g->plast, &v, 0, g->psemi);
    g->plast = v; g->pnv += g->pnv < 2;
}

// VRAM transfers and copies take absolute, unclipped coordinates; sizes of 0 mean the largest, and everything wraps at the edges of VRAM (psx-spx, Masking for COPY
// Commands parameters; Wrapping)
static void xfer_start(ps1_gpu *g, uint32_t pos, uint32_t size, int dir) {
    g->xx = pos & 0x3ff; g->xy = pos >> 16 & 0x1ff;
    g->xw = (uint16_t)((((size & 0xffff) - 1) & 0x3ff) + 1); g->xh = (uint16_t)((((size >> 16) - 1) & 0x1ff) + 1);
    g->xi = 0; g->xfer = dir;
}

static uint16_t *xfer_at(ps1_gpu *g) { return &g->vram[((g->xy + g->xi / g->xw) & 511) * VRAM_W + ((g->xx + g->xi % g->xw) & 1023)]; }

static void pixel_in(ps1_gpu *g, uint16_t v) {
    uint16_t *p = xfer_at(g);
    if (!(g->mask & 2 && *p & 0x8000)) *p = v | (uint16_t)((g->mask & 1) << 15);
    if (++g->xi == (uint32_t)g->xw * g->xh) g->xfer = 0;
}

static uint16_t pixel_out(ps1_gpu *g) {   // after the last pixel, the rest of the word is 0
    if (g->xfer != 2) return 0;
    uint16_t v = *xfer_at(g);
    if (++g->xi == (uint32_t)g->xw * g->xh) g->xfer = 0;
    return v;
}

static void copy(ps1_gpu *g, const uint32_t *w) {
    int sx = w[1] & 0x3ff, sy = w[1] >> 16 & 0x1ff, dx = w[2] & 0x3ff, dy = w[2] >> 16 & 0x1ff;
    int cw = (int)((((w[3] & 0xffff) - 1) & 0x3ff) + 1), ch = (int)((((w[3] >> 16) - 1) & 0x1ff) + 1);
    uint16_t row[VRAM_W];
    for (int y = 0; y < ch; y++) {
        const uint16_t *s = &g->vram[((sy + y) & 511) * VRAM_W];
        uint16_t *d = &g->vram[((dy + y) & 511) * VRAM_W];
        for (int x = 0; x < cw; x++) row[x] = s[(sx + x) & 1023];
        for (int x = 0; x < cw; x++) {
            uint16_t *p = &d[(dx + x) & 1023];
            if (!(g->mask & 2 && *p & 0x8000)) *p = row[x] | (uint16_t)((g->mask & 1) << 15);
        }
    }
}

static void fill(ps1_gpu *g, const uint32_t *w) {   // in 16-pixel steps, ignoring the mask setting and the drawing area (psx-spx, Quick Rectangle Fill)
    int x0 = w[1] & 0x3f0, y0 = w[1] >> 16 & 0x1ff, fw = ((w[2] & 0x3ff) + 0xf) & ~0xfu, fh = w[2] >> 16 & 0x1ff;
    uint16_t c = rgb15(0, 0, w[0] & 0xff, w[0] >> 8 & 0xff, w[0] >> 16 & 0xff, 0);
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++) g->vram[((y0 + y) & 511) * VRAM_W + ((x0 + x) & 1023)] = c;
}

static void env(ps1_gpu *g, uint32_t w) {
    switch (w >> 24) {
    case 0xe1: g->texpage = w & 0x3fff; break;
    case 0xe2: g->texwin = w & 0xfffff; break;
    case 0xe3: g->ax1 = w & 0x3ff; g->ay1 = w >> 10 & 0x3ff; break;
    case 0xe4: g->ax2 = w & 0x3ff; g->ay2 = w >> 10 & 0x3ff; break;
    case 0xe5: g->ox = (int16_t)sext11(w); g->oy = (int16_t)sext11(w >> 11); break;
    case 0xe6: g->mask = w & 3; break;
    default: break;   // E0h, E7h..FFh: no effect
    }
}

static int gp0_len(uint32_t c) {   // the words of a GP0 command, its first included (polylines excepted: they end with a terminator)
    switch (c >> 5) {
    case 0: return c == 0x02 ? 3 : 1;
    case 1: { int n = c & 8 ? 4 : 3; return 1 + n * (c & 4 ? 2 : 1) + (c & 0x10 ? n - 1 : 0); }
    case 2: return c & 0x10 ? 4 : 3;
    case 3: return 2 + (c >> 2 & 1) + !(c & 0x18);
    case 4: return 4;
    case 5: case 6: return 3;
    default: return 1;
    }
}

static void execute(ps1 *m) {
    ps1_gpu *g = &m->gpu;
    uint32_t c = g->buf[0] >> 24;
    if (c >> 5 >= 1 && c >> 5 <= 3 && (g->dmode & 0x24) == 0x24)   // psx-spx, GP1(08h): interlace affects drawing
        RED_ONCE(m, RED_GPU, "gpu: drawing in 480-line interlaced mode draws every line; how interlace limits drawing is not modelled yet (phase 2)");
    switch (c >> 5) {
    case 0:
        if (c == 0x02) fill(g, g->buf);
        else if (c == 0x1f) { g->irq = 1; irq_raise(m, IRQ_GPU); }
        break;   // 01h clears the texture cache, which is not modelled; the others do nothing
    case 1: polygon(m, c); break;
    case 2: {
        int gouraud = c >> 4 & 1;
        ps1_vtx a = vtx(g, g->buf[1], g->buf[0]), b = vtx(g, g->buf[2 + gouraud], g->buf[gouraud ? 2 : 0]);
        line(g, &a, &b, gouraud, c >> 1 & 1);
        break;
    }
    case 3: rectangle(m, c); break;
    case 4: copy(g, g->buf); break;
    case 5: xfer_start(g, g->buf[1], g->buf[2], 1); break;
    case 6: xfer_start(g, g->buf[1], g->buf[2], 2); break;
    default: env(g, g->buf[0]); break;
    }
}

void gpu_gp0(ps1 *m, uint32_t w) {
    ps1_gpu *g = &m->gpu;
    if (g->xfer == 1) { pixel_in(g, (uint16_t)w); if (g->xfer == 1) pixel_in(g, (uint16_t)(w >> 16)); return; }   // an odd count ends on a padding halfword
    if (g->poly) { poly_word(g, w); return; }
    if (!g->n) {
        uint32_t c = w >> 24;
        if ((c & 0xe8) == 0x48) { g->poly = 1; g->pgour = c >> 4 & 1; g->psemi = c >> 1 & 1; g->pnv = 0; g->phave = 1; g->pcol = w; return; }
        g->need = gp0_len(c);
    }
    g->buf[g->n++] = w;
    if (g->n == g->need) { g->n = 0; execute(m); }
}

void gpu_gp1(ps1 *m, uint32_t w) {
    ps1_gpu *g = &m->gpu;
    switch (w >> 24 & 0x3f) {   // 40h..FFh mirror 00h..3Fh
    case 0x00:   // the values of psx-spx, GP1(00h)
        g->n = g->poly = g->xfer = g->irq = g->dma_dir = 0; g->display_off = 1;
        g->disp_x = g->disp_y = 0; g->hx1 = 0x200; g->hx2 = 0x200 + 256 * 10; g->vy1 = 0x10; g->vy2 = 0x10 + 240; g->dmode = 0;
        g->texpage = g->texwin = g->mask = 0; g->ax1 = g->ay1 = g->ax2 = g->ay2 = g->ox = g->oy = 0;
        break;
    case 0x01: g->n = g->poly = g->xfer = 0; break;
    case 0x02: g->irq = 0; break;
    case 0x03: g->display_off = w & 1; break;
    case 0x04: g->dma_dir = w & 3; break;
    case 0x05: g->disp_x = w & 0x3ff; g->disp_y = w >> 10 & 0x1ff; break;
    case 0x06: g->hx1 = w & 0xfff; g->hx2 = w >> 12 & 0xfff; break;
    case 0x07: g->vy1 = w & 0x3ff; g->vy2 = w >> 10 & 0x3ff; break;
    case 0x08: g->dmode = w & 0xff; break;
    case 0x10 ... 0x1f:   // latch a register for GPUREAD, as a v2 GPU does; the other indices leave the latch alone
        switch (w & 0xf) {
        case 2: g->read = g->texwin; break;
        case 3: g->read = (uint32_t)g->ax1 | (uint32_t)g->ay1 << 10; break;
        case 4: g->read = (uint32_t)g->ax2 | (uint32_t)g->ay2 << 10; break;
        case 5: g->read = (g->ox & 0x7ffu) | (g->oy & 0x7ffu) << 11; break;
        case 7: g->read = 2; break;
        case 8: g->read = 0; break;
        default: break;
        }
        break;
    default: break;
    }
}

uint32_t gpu_read(ps1 *m) {
    ps1_gpu *g = &m->gpu;
    if (g->xfer == 2) { uint32_t lo = pixel_out(g); g->read = lo | (uint32_t)pixel_out(g) << 16; }
    return g->read;
}

// Drawing is instant, so the GPU is always ready for commands and DMA. Bit 31 is the field in 480-line mode and the scanline's parity otherwise, 0 in vertical blank
uint32_t gpu_stat(ps1 *m) {
    ps1_gpu *g = &m->gpu;
    uint32_t d = g->dmode, il = d >> 5 & 1, vblank = g->line < g->vy1 || g->line >= g->vy2;
    uint32_t odd = vblank ? 0 : (d & 0x24) == 0x24 ? (uint32_t)g->field : (uint32_t)g->line & 1;
    uint32_t s = (g->texpage & 0x7ff) | (g->mask & 3) << 11 | (uint32_t)(!il || g->field) << 13 | (d >> 7 & 1) << 14 | (g->texpage >> 11 & 1) << 15 |
                 (d >> 6 & 1) << 16 | (d & 3) << 17 | (d >> 2 & 1) << 19 | (d >> 3 & 1) << 20 | (d >> 4 & 1) << 21 | il << 22 |
                 (uint32_t)g->display_off << 23 | (uint32_t)g->irq << 24 | 1u << 26 | (uint32_t)(g->xfer == 2) << 27 | 1u << 28 | (uint32_t)g->dma_dir << 29 | odd << 31;
    uint32_t req = g->dma_dir == 0 ? 0 : g->dma_dir == 1 ? 1 : g->dma_dir == 2 ? s >> 28 & 1 : s >> 27 & 1;
    return s | req << 25;
}

static uint64_t line_fp(const ps1_gpu *g) {   // CPU cycles per scanline, 16.16: 3413 or 3406 cycles of the NTSC or PAL video clock (psx-spx, GPU Timings)
    return g->dmode & 8 ? 3406ull * CPU_HZ * 65536 / 53203425 : 3413ull * CPU_HZ * 65536 / 53693175;
}

static int field_lines(const ps1_gpu *g) {   // 263 or 314 lines; interlaced, 262.5 or 312.5 on average
    int pal = g->dmode >> 3 & 1;
    return g->dmode & 0x20 ? (pal ? 312 : 262) + g->field : pal ? 314 : 263;
}

uint64_t gpu_dot_fp(const ps1 *m) {
    static const uint64_t divider[4] = {10, 8, 5, 4};
    uint32_t d = m->gpu.dmode;
    return (d & 0x40 ? 7 : divider[d & 3]) * CPU_HZ * 65536 / (d & 8 ? 53203425 : 53693175);
}

// The vertical blank starts at the display range's last line, GP1(07h) Y2, with IRQ 0; a Y2 past the field's last line never starts it (psx-spx, GP1(07h))
void gpu_line(ps1 *m) {
    ps1_gpu *g = &m->gpu;
    uint64_t p = line_fp(g) + g->line_frac;
    g->next_line += p >> 16; g->line_frac = p & 0xffff;
    tmr_hblank(m);
    int lines = field_lines(g);
    if (++g->line >= lines) {
        g->line = 0; g->field = g->dmode & 0x20 ? !g->field : 0;
        if (g->vy2 >= lines) m->frame_done = 1;
    }
    if (g->line == g->vy2) { irq_raise(m, IRQ_VBLANK); m->frame_done = 1; }
}

void gpu_reset(ps1 *m) {   // VRAM keeps its contents, as the other memories do
    ps1_gpu *g = &m->gpu;
    memset((char *)g + offsetof(ps1_gpu, buf), 0, sizeof *g - offsetof(ps1_gpu, buf));
    gpu_gp1(m, 0);
    uint64_t p = line_fp(g);
    g->next_line = m->cycles + (p >> 16); g->line_frac = p & 0xffff;
}

static uint32_t expand5(uint32_t v) { return v << 3 | v >> 2; }

// The display area at its nominal size: 256, 320, 368, 512 or 640 pixels by GP1(07h)'s line count (at most 240 or 288), doubled in 480-line mode. The horizontal range
// of GP1(06h) only places the picture on a TV, so it does not crop it here
void ps1_display(const ps1 *m, uint32_t *out, int cap_w, int cap_h, int *w, int *h) {
    static const int widths[4] = {256, 320, 512, 640};
    const ps1_gpu *g = &m->gpu;
    uint32_t d = g->dmode;
    int lines = g->vy2 > g->vy1 ? g->vy2 - g->vy1 : 0, pw = d & 0x40 ? 368 : widths[d & 3];
    lines = MIN(lines, d & 8 ? 288 : 240);
    int ph = (d & 0x24) == 0x24 ? lines * 2 : lines;
    pw = MIN(pw, cap_w); ph = MIN(ph, cap_h);
    *w = pw; *h = ph;
    for (int y = 0; y < ph; y++) {
        uint32_t *o = out + (size_t)y * (size_t)pw;
        const uint16_t *row = &g->vram[((g->disp_y + y) & 511) * VRAM_W];
        for (int x = 0; x < pw; x++) {
            if (g->display_off) { o[x] = 0xff000000; continue; }
            if (d & 0x10) {   // 24-bit: three bytes a pixel, from the display start's halfword
                uint32_t k = g->disp_x * 2u + (uint32_t)x * 3, rgb = 0;
                for (uint32_t i = 0; i < 3; i++) rgb |= (uint32_t)(row[((k + i) >> 1) & 1023] >> ((k + i) & 1) * 8 & 0xff) << (16 - 8 * i);
                o[x] = 0xff000000 | rgb;
            } else {
                uint16_t p = row[(g->disp_x + x) & 1023];
                o[x] = 0xff000000 | expand5(p & 31) << 16 | expand5(p >> 5 & 31) << 8 | expand5(p >> 10 & 31);
            }
        }
    }
}

uint64_t ps1_frame_hash(const uint32_t *px, int w, int h) {
    uint64_t hash = 0xcbf29ce484222325;
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++) hash = (hash ^ px[i]) * 0x100000001b3;
    return hash;
}
