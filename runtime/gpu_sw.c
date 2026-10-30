// The GPU (psx-spx, Graphics Processing Unit): GP0 commands draw into the 1 MB VRAM and move rectangles in and out of it, GP1 commands set up the display, and GPUSTAT
// reports both. Drawing is done in software as each command completes: textured/untextured polygons (flat or gouraud), lines and polylines, rectangles, fills and copies, with
// semi-transparency, dithering and the mask bit. The video timing (GPU Timings) drives the scanline events: hblanks for timer 1, the blanks the timers synchronize to,
// the vertical blank IRQ, the end of a frame. Public reference: https://psx-spx.consoledev.net/graphicsprocessingunitgpu/ . Not modelled: the time drawing takes, and
// which lines 480-line interlaced mode keeps drawing from (psx-spx does not say)
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
    if (semi) c = (c & 0x8000) | blend(*p, c, g->texpage >> 5 & 3);
    *p = c | (uint16_t)((g->mask & 1) << 15);
}

static ps1_vtx vtx(const ps1_gpu *g, uint32_t pos, uint32_t col) {   // 11-bit signed coordinates, moved by the drawing offset
    return (ps1_vtx){sext11(pos) + g->ox, sext11(pos >> 16) + g->oy, (int)(col & 0xff), (int)(col >> 8 & 0xff), (int)(col >> 16 & 0xff), 0, 0};
}

// The textured commands observed during the disc smoke run were discarded. t_texture gives them synthetic pages and CLUTs: GPU Video Memory, Texture Bitmaps/Palettes;
// GP0(E2h) replaces selected UV bits before addressing, and GP0(E6h) preserves the texel's bit15 even when blending. Zero means transparent only after CLUT lookup
static uint16_t texel(ps1_gpu *g, int u, int v, uint32_t clut) {
    uint32_t tw = g->texwin; int mx = (tw & 31) * 8, my = (tw >> 5 & 31) * 8;
    u = ((u & 255) & ~mx) | ((int)(tw >> 10 & 31) * 8 & mx);
    v = ((v & 255) & ~my) | ((int)(tw >> 15 & 31) * 8 & my);
    int depth = g->texpage >> 7 & 3, shift = depth == 0 ? 2 : depth == 1 ? 1 : 0;
    int x = ((int)(g->texpage & 15) * 64 + (u >> shift)) & 1023, y = ((int)(g->texpage >> 4 & 1) * 256 + v) & 511;
    // GPU Texture Caching: each of 256 entries contains four VRAM words. 4/8-bit blocks have four entries per row, 15-bit blocks eight; colliding blocks replace a slot
    unsigned slot = shift ? (unsigned)((x >> 2 & 3) | (y & 63) << 2) : (unsigned)((x >> 2 & 7) | (y & 31) << 3);
    uint32_t tag = (uint32_t)(y * VRAM_W + (x & ~3));
    if (g->textag[slot] != tag + 1) { memcpy(g->texcache[slot], g->vram + tag, 8); g->textag[slot] = tag + 1; }
    uint16_t c = g->texcache[slot][x & 3];
    if (!shift) return c;
    unsigned index = shift == 2 ? c >> ((u & 3) * 4) & 15 : c >> ((u & 1) * 8) & 255;
    clut &= 0x7fff;
    // The CLUT cache takes the entries the depth uses, 16 or 256: the hardware VRAM of ps1-tests gpu/clut-cache shows an 8-bit texture reloading a CLUT that a 4-bit
    // one cached, and a 4-bit texture keeping one that an 8-bit one cached
    int need = shift == 2 ? 16 : 256;
    if (!g->clut_valid || g->clut_id != clut || g->clut_n < need) {
        for (int i = 0; i < need; i++) g->clutcache[i] = g->vram[(clut >> 6) * VRAM_W + (((clut & 63) * 16 + i) & 1023)];
        g->clut_id = (uint16_t)clut; g->clut_valid = 1; g->clut_n = need;
    }
    return g->clutcache[index];
}

static void texture_plot(ps1_gpu *g, int x, int y, int u, int v, uint32_t clut, int r, int gr, int b, int raw, int semi, int dither) {
    uint16_t c = texel(g, u, v, clut);
    if (!c) return;
    uint16_t mask = c & 0x8000;
    // GPU (MISC), Modulation: 80h is neutral, FFh nearly doubles brightness. Keep 8-bit precision until dithering, then saturate to framebuffer channels
    if (!raw) c = rgb15(x, y, clamp8((c & 31) * r >> 4), clamp8((c >> 5 & 31) * gr >> 4), clamp8((c >> 10 & 31) * b >> 4), dither) | mask;
    plot(g, x, y, c, semi && mask);
}

enum { FRAC = 12 };   // the fraction bits of an interpolation gradient (triangle)
static int64_t edge(const ps1_vtx *p, const ps1_vtx *q, int x, int y) { return (int64_t)(q->x - p->x) * (y - p->y) - (int64_t)(q->y - p->y) * (x - p->x); }
static int owns(const ps1_vtx *p, const ps1_vtx *q) { return q->y < p->y || (q->y == p->y && q->x > p->x); }   // top and left edges keep the pixels on them

// Polygons leave out their right and bottom edges, are not drawn when wider than 1023 or taller than 511, and are dithered only when shaded (psx-spx, Vertex; Notes).
// Colours and texture coordinates step from the first vertex by gradients with 12 fraction bits, truncated, and round to the nearest value: psx-spx gives no precision,
// and this is the one that reproduces the hardware VRAM of ps1-tests gpu/triangle, gpu/quad and gpu/uv-interpolation exactly (8, 10, 14, 16 and 20 bits, or
// gradients rounded down, leave hundreds to thousands of pixels different)
static void triangle(ps1_gpu *g, ps1_vtx a, ps1_vtx b, ps1_vtx c, int gouraud, int semi, int tex, int raw, uint32_t clut) {
    int x0 = MIN(MIN(a.x, b.x), c.x), x1 = MAX(MAX(a.x, b.x), c.x), y0 = MIN(MIN(a.y, b.y), c.y), y1 = MAX(MAX(a.y, b.y), c.y);
    if (x1 - x0 > 1023 || y1 - y0 > 511) return;
    int64_t area = edge(&a, &b, c.x, c.y);
    if (!area) return;
    if (area < 0) { ps1_vtx t = b; b = c; c = t; area = -area; }
    x0 = MAX(x0, g->ax1); x1 = MIN(x1, g->ax2); y0 = MAX(y0, g->ay1); y1 = MIN(y1, g->ay2);
    int dither = (gouraud || (tex && !raw)) && g->texpage >> 9 & 1;
    uint16_t flat = rgb15(0, 0, a.r, a.g, a.b, 0);
    int64_t bx = b.x - a.x, by = b.y - a.y, cx = c.x - a.x, cy = c.y - a.y, dx[5], dy[5];
    const int fa[5] = {a.r, a.g, a.b, a.u, a.v}, fb[5] = {b.r, b.g, b.b, b.u, b.v}, fc[5] = {c.r, c.g, c.b, c.u, c.v};
    for (int i = 0; i < 5; i++) {
        int64_t nb = fb[i] - fa[i], nc = fc[i] - fa[i];
        dx[i] = (nb * cy - nc * by) * (1 << FRAC) / area; dy[i] = (nc * bx - nb * cx) * (1 << FRAC) / area;
    }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int64_t wa = edge(&b, &c, x, y), wb = edge(&c, &a, x, y), wc = edge(&a, &b, x, y);
            if (wa < 0 || wb < 0 || wc < 0 || (!wa && !owns(&b, &c)) || (!wb && !owns(&c, &a)) || (!wc && !owns(&a, &b))) continue;
#define LERP(i) (int)(((int64_t)fa[i] * (1 << FRAC) + dx[i] * (x - a.x) + dy[i] * (y - a.y) + (1 << (FRAC - 1))) >> FRAC)
            int r = a.r, gr = a.g, bl = a.b;
            if (gouraud) { r = LERP(0); gr = LERP(1); bl = LERP(2); }
            if (tex) texture_plot(g, x, y, LERP(3), LERP(4), clut, r, gr, bl, raw, semi, dither);
            else plot(g, x, y, gouraud ? rgb15(x, y, r, gr, bl, dither) : flat, semi);
#undef LERP
        }
}

static void dot(ps1_gpu *g, int x, int y, int r, int gr, int b, int semi, int dither) {
    if (x >= g->ax1 && x <= g->ax2 && y >= g->ay1 && y <= g->ay2) plot(g, x, y, rgb15(x, y, r, gr, b, dither), semi);
}

// Lines include both ends and are always dithered when dithering is on, even when both ends are one pixel (psx-spx, GPU Render Line Commands). The hardware VRAM of
// ps1-tests gpu/lines fixes the steps: along the longer axis one pixel at a time, the other axis rounded to the nearest pixel, a tie stepping a mostly horizontal line
// (and holding a mostly vertical one) at the halfway point; gouraud colours step like a polygon's (triangle). Only lines drawn rightwards and downwards show ties there,
// so the other directions follow the same rule by symmetry
static void line(ps1_gpu *g, const ps1_vtx *a, const ps1_vtx *b, int gouraud, int semi) {
    int dx = b->x - a->x, dy = b->y - a->y, n = MAX(abs(dx), abs(dy));
    if (abs(dx) > 1023 || abs(dy) > 511) return;
    int dither = g->texpage >> 9 & 1;
    if (!n) { dot(g, a->x, a->y, a->r, a->g, a->b, semi, dither); return; }
    int xmajor = abs(dx) >= abs(dy), d = xmajor ? dy : dx, sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    for (int i = 0; i <= n; i++) {
        int r = a->r, gr = a->g, bl = a->b;
#define LERP(f) (int)(((int64_t)a->f * (1 << FRAC) + i * ((int64_t)(b->f - a->f) * (1 << FRAC) / n) + (1 << (FRAC - 1))) >> FRAC)
        if (gouraud) { r = LERP(r); gr = LERP(g); bl = LERP(b); }
#undef LERP
        int off = (int)((2 * (int64_t)i * abs(d) + n - !xmajor) / (2 * n));
        int x = xmajor ? a->x + sx * i : a->x + sx * off, y = xmajor ? a->y + sy * off : a->y + sy * i;
        dot(g, x, y, r, gr, bl, semi, dither);
    }
}

static void polygon(ps1 *m, uint32_t c) {
    ps1_gpu *g = &m->gpu;
    const uint32_t *w = g->buf;
    int gouraud = c >> 4 & 1, n = c & 8 ? 4 : 3, tex = c >> 2 & 1, p = 1;
    ps1_vtx v[4]; uint32_t clut = 0;
    for (int k = 0; k < n; k++) {
        uint32_t col = gouraud && k ? w[p++] : w[0];
        v[k] = vtx(g, w[p++], col);
        if (tex) { v[k].u = w[p] & 255; v[k].v = w[p] >> 8 & 255; if (!k) clut = w[p] >> 16; }
        if (tex && k == 1) g->texpage = (g->texpage & ~0x9ffu) | (w[p] >> 16 & (g->tex_disable_ok ? 0x9ff : 0x1ff));   // the page attribute sets GP0(E1h) bits 0-8 and 11
        p += tex;
    }
    triangle(g, v[0], v[1], v[2], gouraud, c >> 1 & 1, tex, c & 1, clut);
    if (n == 4) triangle(g, v[1], v[2], v[3], gouraud, c >> 1 & 1, tex, c & 1, clut);   // a quad is the triangles 1-2-3 and 2-3-4
}

static void rectangle(ps1 *m, uint32_t c) {
    static const int side[4] = {0, 1, 8, 16};
    ps1_gpu *g = &m->gpu;
    int tex = c >> 2 & 1, w = side[c >> 3 & 3], h = w;
    if (!w) { w = g->buf[2 + tex] & 0x3ff; h = g->buf[2 + tex] >> 16 & 0x1ff; }
    ps1_vtx v = vtx(g, g->buf[1], g->buf[0]);
    uint16_t col = rgb15(0, 0, v.r, v.g, v.b, 0);
    // GP0(E1h) bits 12/13 flip rectangles' textures. The hardware VRAM of ps1-tests gpu/texture-flip shows U running u0+1, u0, u0-1... across an X-flipped rectangle
    // and V running v0, v0-1... down a Y-flipped one; polygons ignore both bits
    for (int y = MAX(v.y, g->ay1); y <= MIN(v.y + h - 1, g->ay2); y++)
        for (int x = MAX(v.x, g->ax1); x <= MIN(v.x + w - 1, g->ax2); x++) {
            if (tex) texture_plot(g, x, y, (int)(g->buf[2] & 255) + (g->texpage & 0x1000 ? v.x - x + 1 : x - v.x),
                (int)(g->buf[2] >> 8 & 255) + (g->texpage & 0x2000 ? v.y - y : y - v.y), g->buf[2] >> 16, v.r, v.g, v.b, c & 1, c >> 1 & 1, 0);
            else plot(g, x, y, col, c >> 1 & 1);
        }
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

static void env(ps1 *m, uint32_t w) {
    ps1_gpu *g = &m->gpu;
    switch (w >> 24) {
    case 0xe1:   // bit 11 only after GP1(09h) allows it: the hardware log of ps1-tests gpu/gp0-e1 keeps GPUSTAT.15 clear otherwise
        g->texpage = w & (g->tex_disable_ok ? 0x3fff : 0x37ff);
        if (g->texpage & 0x800) RED_ONCE(m, RED_TEX_DISABLE, "gpu: textures disabled through GP0(E1h) bit 11: textured primitives still draw their texture");
        break;
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
    if (c >> 5 >= 1 && c >> 5 <= 3 && (g->dmode & 0x24) == 0x24)   // psx-spx, GP1(08h): "the Vertical Interlace flag DOES affect GP0 draw commands", without saying how
        RED_ONCE(m, RED_GPU, "gpu: drawing in 480-line interlaced mode draws every line; which lines interlace keeps it from drawing is not documented");
    switch (c >> 5) {
    case 0:
        if (c == 0x02) fill(g, g->buf);
        else if (c == 0x01) { memset(g->textag, 0, sizeof g->textag); g->clut_valid = 0; }
        else if (c == 0x1f) { g->irq = 1; irq_raise(m, IRQ_GPU); }
        break;
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
    default: env(m, g->buf[0]); break;
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

// The blanks the timers synchronize to (psx-spx, Timers, MODE). The vertical blank is the lines outside GP1(07h)'s display range Y1..Y2, as for IRQ 0. Timer 0's hblank is
// the part of a scanline outside GP1(06h)'s range X1..X2, which counts video cycles from HSYNC, here the scanline's first cycle: "a display configured to 0 pixels
// width would cause an endless hblank" (psx-spx, GPU Timings, Vertical Timings). The ps1-tests timers log agrees: with the range 260h..C60h, timer 0 synchronized to
// hblanks pauses for about 540 of the 2150 cycles of an NTSC scanline
static int in_vblank(const ps1_gpu *g) { return g->line < g->vy1 || g->line >= g->vy2; }
int gpu_vblank(const ps1 *m) { return in_vblank(&m->gpu); }

static uint64_t hpos(const ps1_gpu *g, uint32_t x) { return (uint64_t)x * CPU_HZ / (g->dmode & 8 ? 53203425 : 53693175); }   // video cycles to CPU cycles
static int in_hblank(const ps1_gpu *g, uint64_t pos) { return pos < hpos(g, g->hx1) || pos >= hpos(g, g->hx2); }   // pos: CPU cycles into the scanline

static void hblank_plan(ps1 *m, uint64_t pos) {   // the scanline's next hblank edge after pos, while timer 0 synchronizes
    ps1_gpu *g = &m->gpu; uint64_t x1 = hpos(g, g->hx1), x2 = hpos(g, g->hx2), len = g->next_line - g->line_start;
    g->next_hblank = NEVER;
    if (!(m->tmr[0].mode & 1) || x1 >= x2) return;
    if (pos < x1 && x1 < len) g->next_hblank = g->line_start + x1;
    else if (pos < x2 && x2 < len) g->next_hblank = g->line_start + x2;
}

int gpu_hblank_now(ps1 *m) {
    ps1_gpu *g = &m->gpu; uint64_t pos = m->cycles - g->line_start;
    g->hblank = in_hblank(g, pos); hblank_plan(m, pos);
    return g->hblank;
}

void gpu_hblank(ps1 *m) {
    ps1_gpu *g = &m->gpu; uint64_t at = g->next_hblank, pos = at - g->line_start;
    g->hblank = in_hblank(g, pos); hblank_plan(m, pos);
    tmr_blank(m, 0, g->hblank, at);
}

static void blank_edges(ps1 *m, int vblank, uint64_t at) {   // after the scanline or the display range changed: the edges they made
    ps1_gpu *g = &m->gpu;
    if (in_vblank(g) != vblank) tmr_blank(m, 1, !vblank, at);
    if (!(m->tmr[0].mode & 1)) { g->next_hblank = NEVER; return; }
    uint64_t pos = at - g->line_start; int h = in_hblank(g, pos);
    hblank_plan(m, pos);
    if (h != g->hblank) { g->hblank = h; tmr_blank(m, 0, h, at); }
}

void gpu_gp1(ps1 *m, uint32_t w) {
    ps1_gpu *g = &m->gpu;
    int vblank = in_vblank(g); uint32_t c = w >> 24 & 0x3f;
    switch (c) {   // 40h..FFh mirror 00h..3Fh
    case 0x00:   // the values of psx-spx, GP1(00h)
        memset(g->textag, 0, sizeof g->textag); g->clut_valid = 0;
        g->n = g->poly = g->xfer = g->irq = g->dma_dir = 0; g->display_off = 1;
        g->disp_x = g->disp_y = 0; g->hx1 = 0x200; g->hx2 = 0x200 + 256 * 10; g->vy1 = 0x10; g->vy2 = 0x10 + 240; g->dmode = 0;
        g->texpage = g->texwin = g->mask = 0; g->ax1 = g->ay1 = g->ax2 = g->ay2 = g->ox = g->oy = 0;
        break;
    case 0x01: g->n = g->poly = g->xfer = 0; g->clut_valid = 0; break;   // GP1(01h) also resets CLUT cache
    case 0x02: g->irq = 0; break;
    case 0x03: g->display_off = w & 1; break;
    case 0x04: g->dma_dir = w & 3; break;
    case 0x05: g->disp_x = w & 0x3ff; g->disp_y = w >> 10 & 0x1ff; break;
    case 0x06: g->hx1 = w & 0xfff; g->hx2 = w >> 12 & 0xfff; break;
    case 0x07: g->vy1 = w & 0x3ff; g->vy2 = w >> 10 & 0x3ff; break;
    case 0x08: g->dmode = w & 0xff; break;
    case 0x09: g->tex_disable_ok = w & 1; break;
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
    if (c == 0 || (c >= 6 && c <= 8)) blank_edges(m, vblank, m->cycles);   // GP1(00h) and (06h..08h) move the blanks
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

// The vertical blank starts at the display range's last line, GP1(07h) Y2, with IRQ 0; a Y2 past the field's last line never starts it (psx-spx, GP1(07h)). Timer 1
// counts an hblank a scanline, none when the hblank never ends or never starts (psx-spx, GPU Timings, Vertical Timings)
void gpu_line(ps1 *m) {
    ps1_gpu *g = &m->gpu;
    int vblank = in_vblank(g);
    uint64_t p = line_fp(g) + g->line_frac, x1 = hpos(g, g->hx1), x2 = hpos(g, g->hx2);
    g->line_start = g->next_line; g->next_line += p >> 16; g->line_frac = p & 0xffff;
    if (x1 < x2 && (x1 > 0 || x2 < g->next_line - g->line_start)) tmr_hblank(m);
    int lines = field_lines(g);
    if (++g->line >= lines) {
        g->line = 0; g->field = g->dmode & 0x20 ? !g->field : 0;
        if (g->vy2 >= lines) m->frame_done = 1;
    }
    if (g->line == g->vy2) { irq_raise(m, IRQ_VBLANK); m->frame_done = 1; }
    blank_edges(m, vblank, g->line_start);
}

void gpu_reset(ps1 *m) {   // VRAM keeps its contents, as the other memories do
    ps1_gpu *g = &m->gpu;
    memset((char *)g + offsetof(ps1_gpu, buf), 0, sizeof *g - offsetof(ps1_gpu, buf));
    g->line_start = m->cycles; g->next_hblank = NEVER;
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
