// Integer GPU rendering (plan.md, phase 6), from our MIT software reference and public psx-spx GPU documentation. Native coverage/interpolation deliberately uses
// the reference's integer edges and 12-bit gradients: ordinary floating-point triangle rasterization does not reproduce its VRAM. Texture/cache mutations and
// overlapping copies run in command/scanline order. A separate parallel pass samples geometry at subpixels for 2x/4x; CPU-visible VRAM always stays at native size.
#include "../../runtime/render.h"

constant uint SAMPLE_BASE = 32;
constant uint WRITTEN_BASE = SAMPLE_BASE + 1024 * 512;
constant int dither_offsets[16] = {-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2};

static int clamp8(int v) { return clamp(v, 0, 255); }
static uint log_scale(uint s) { return s == 4 ? 2 : s == 2 ? 1 : 0; }
static uint index_at(int x, int y, uint s) { return (uint(y) & (512 * s - 1)) * (1024 * s) + (uint(x) & (1024 * s - 1)); }
static ushort rgb15(int x, int y, int r, int g, int b, bool dither) {
    if (dither) { int n = dither_offsets[(y & 3) * 4 + (x & 3)]; r = clamp8(r + n); g = clamp8(g + n); b = clamp8(b + n); }
    return ushort((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10);
}
static ushort blend(ushort back, ushort front, uint mode) {
    uint out = 0;
    for (uint s = 0; s < 15; s += 5) {
        int b = back >> s & 31, f = front >> s & 31;
        int v = mode == 0 ? (b + f) >> 1 : mode == 1 ? b + f : mode == 2 ? b - f : b + (f >> 2);
        out |= uint(clamp(v, 0, 31)) << s;
    }
    return ushort(out);
}
static void plot(device ushort *vram, int x, int y, uint s, ushort color, bool semi, uint mask, uint page) {
    device ushort &p = vram[index_at(x, y, s)];
    if ((mask & 2) && (p & 0x8000)) return;
    if (semi) color = (color & 0x8000) | blend(p, color, page >> 5 & 3);
    p = color | ushort((mask & 1) << 15);
}
static int2 window_uv(int u, int v, uint window) {
    int mx = (window & 31) * 8, my = (window >> 5 & 31) * 8;
    return int2(((u & 255) & ~mx) | ((int(window >> 10 & 31) * 8) & mx), ((v & 255) & ~my) | ((int(window >> 15 & 31) * 8) & my));
}
static uint4 texture_address(int u, int v, uint page, uint window) {
    int2 uv = window_uv(u, v, window); int depth = page >> 7 & 3, shift = depth == 0 ? 2 : depth == 1 ? 1 : 0;
    uint x = ((page & 15) * 64 + (uint(uv.x) >> shift)) & 1023, y = ((page >> 4 & 1) * 256 + uint(uv.y)) & 511;
    uint slot = shift ? ((x >> 2 & 3) | (y & 63) << 2) : ((x >> 2 & 7) | (y & 31) << 3);
    return uint4(x, y, slot, shift);
}
static ushort texel(device const ushort *vram, device ps1_render_cache &cache, int u, int v, constant ps1_render_op &o) {
    uint4 a = texture_address(u, v, o.texpage, o.texwin); uint tag = a.y * 1024 + (a.x & ~3u), slot = a.z, shift = a.w;
    if (cache.textag[slot] != tag + 1) { for (uint i = 0; i < 4; i++) cache.texcache[slot][i] = vram[tag + i]; cache.textag[slot] = tag + 1; }
    ushort c = cache.texcache[slot][a.x & 3]; if (!shift) return c;
    int2 uv = window_uv(u, v, o.texwin); uint entry = shift == 2 ? c >> ((uv.x & 3) * 4) & 15 : c >> ((uv.x & 1) * 8) & 255;
    uint clut = o.clut & 0x7fff; int need = shift == 2 ? 16 : 256;
    if (!cache.clut_valid || cache.clut_id != clut || cache.clut_n < need) {
        for (int i = 0; i < need; i++) cache.clutcache[i] = vram[(clut >> 6) * 1024 + (((clut & 63) * 16 + uint(i)) & 1023)];
        cache.clut_id = ushort(clut); cache.clut_valid = 1; cache.clut_n = need;
    }
    return cache.clutcache[entry];
}
// Extra subpixel samples do not mutate the native texture caches. When a native sample addresses the same texel, keep its observed value, including stale cache
// contents. Otherwise consult the current cache without changing its tags, falling back to the native texture page for the additional coordinate.
static ushort texel_extra(device const ushort *vram, device const ps1_render_cache &cache, int u, int v, constant ps1_render_op &o) {
    uint4 a = texture_address(u, v, o.texpage, o.texwin); uint tag = a.y * 1024 + (a.x & ~3u);
    ushort c = cache.textag[a.z] == tag + 1 ? cache.texcache[a.z][a.x & 3] : vram[a.y * 1024 + a.x];
    if (!a.w) return c;
    int2 uv = window_uv(u, v, o.texwin); uint entry = a.w == 2 ? c >> ((uv.x & 3) * 4) & 15 : c >> ((uv.x & 1) * 8) & 255;
    uint clut = o.clut & 0x7fff, need = a.w == 2 ? 16 : 256;
    if (cache.clut_valid && cache.clut_id == clut && uint(cache.clut_n) >= need) return cache.clutcache[entry];
    return vram[(clut >> 6) * 1024 + (((clut & 63) * 16 + entry) & 1023)];
}
static void textured_plot(device ushort *vram, int x, int y, uint scale, ushort c, int r, int g, int b, constant ps1_render_op &o) {
    if (!c) return;
    ushort mask = c & 0x8000;
    if (!(o.flags & RENDER_RAW)) c = rgb15(x / int(scale), y / int(scale), clamp8((c & 31) * r >> 4), clamp8((c >> 5 & 31) * g >> 4), clamp8((c >> 10 & 31) * b >> 4), o.flags & RENDER_DITHER) | mask;
    plot(vram, x, y, scale, c, (o.flags & RENDER_SEMI) && mask, o.mask, o.texpage);
}
static long edge(ps1_vtx a, ps1_vtx b, long x, long y, uint scale) {
    return long(b.x - a.x) * (y - long(a.y) * scale) - long(b.y - a.y) * (x - long(a.x) * scale);
}
static bool owns(ps1_vtx a, ps1_vtx b) { return b.y < a.y || (b.y == a.y && b.x > a.x); }
static bool inside(ps1_vtx a, ps1_vtx b, ps1_vtx c, int x, int y, uint scale) {
    long wa = edge(b, c, x, y, scale), wb = edge(c, a, x, y, scale), wc = edge(a, b, x, y, scale);
    return wa >= 0 && wb >= 0 && wc >= 0 && (wa || owns(b, c)) && (wb || owns(c, a)) && (wc || owns(a, b));
}
static int4 bounds(constant ps1_render_op &o) {
    int x0, x1, y0, y1;
    if (o.type == RENDER_RECT) { x0 = o.a.x; y0 = o.a.y; x1 = x0 + int(o.p[0]) - 1; y1 = y0 + int(o.p[1]) - 1; }
    else { x0 = min(o.a.x, o.b.x); x1 = max(o.a.x, o.b.x); y0 = min(o.a.y, o.b.y); y1 = max(o.a.y, o.b.y);
        if (o.type == RENDER_TRIANGLE) { x0 = min(x0, o.c.x); x1 = max(x1, o.c.x); y0 = min(y0, o.c.y); y1 = max(y1, o.c.y); }
    }
    return int4(max(x0, o.ax1), max(y0, o.ay1), min(x1, o.ax2), min(y1, o.ay2));
}
static void gradients(ps1_vtx a, ps1_vtx b, ps1_vtx c, long area, device long *grad) {
    int fa[5] = {a.r, a.g, a.b, a.u, a.v}, fb[5] = {b.r, b.g, b.b, b.u, b.v}, fc[5] = {c.r, c.g, c.b, c.u, c.v};
    long bx = b.x - a.x, by = b.y - a.y, cx = c.x - a.x, cy = c.y - a.y;
    for (uint i = 0; i < 5; i++) {
        long nb = fb[i] - fa[i], nc = fc[i] - fa[i]; grad[i] = (nb * cy - nc * by) * 4096 / area; grad[i + 5] = (nc * bx - nb * cx) * 4096 / area;
    }
}
static int interpolate(int value, ps1_vtx a, int x, int y, uint scale, device const long *grad, uint i) {
    long n = long(value) * 4096 * scale + grad[i] * (x - a.x * int(scale)) + grad[i + 5] * (y - a.y * int(scale)) + 2048 * scale;
    return int(n >> (12 + log_scale(scale)));
}
static void save_sample(device uint *scratch, device const ushort *vram, int x, int y, int u, int v, ushort sample, constant ps1_render_op &o) {
    uint at = index_at(x, y, 1); int2 uv = window_uv(u, v, o.texwin);
    scratch[SAMPLE_BASE + at] = uint(sample) | uint(uv.x) << 16 | uint(uv.y) << 24;
    // A feedback draw can sample transparency or hit a protected destination. Its native pixel then keeps old data; copying it into the finer target would erase
    // preexisting subpixel detail. Remember actual writes separately from the texel/UV word. Native textured rasterization is ordered, so these bit updates cannot race.
    uint bit = 1u << (at & 31), old = scratch[WRITTEN_BASE + (at >> 5)];
    bool wrote = sample && !((o.mask & 2) && (vram[at] & 0x8000));
    scratch[WRITTEN_BASE + (at >> 5)] = wrote ? old | bit : old & ~bit;
}
static bool sample_written(device const uint *scratch, int x, int y) {
    uint at = index_at(x, y, 1); return scratch[WRITTEN_BASE + (at >> 5)] & (1u << (at & 31));
}
static bool intervals_overlap(uint a, uint an, uint b, uint bn, uint size) {
    return an && bn && (((a - b) & (size - 1)) < bn || ((b - a) & (size - 1)) < an);
}
static bool texture_feedback(constant ps1_render_op &o, int4 box) {
    uint width = uint(box.z - box.x + 1), height = uint(box.w - box.y + 1), depth = o.texpage >> 7 & 3;
    uint tx = (o.texpage & 15) * 64, ty = (o.texpage >> 4 & 1) * 256, tw = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    if (intervals_overlap(uint(box.x), width, tx, tw, 1024) && intervals_overlap(uint(box.y), height, ty, 256, 512)) return true;
    uint clut = o.clut & 0x7fff, entries = depth == 0 ? 16 : 256;
    return depth < 2 && intervals_overlap(uint(box.x), width, (clut & 63) * 16, entries, 1024) && intervals_overlap(uint(box.y), height, clut >> 6, 1, 512);
}

static void draw_triangle(device ushort *vram, device ps1_render_cache &cache, constant ps1_render_op &o, device uint *scratch, uint scale) {
    ps1_vtx a = o.a, b = o.b, c = o.c; long area = edge(a, b, c.x, c.y, 1);
    if (!area || max(max(a.x, b.x), c.x) - min(min(a.x, b.x), c.x) > 1023 || max(max(a.y, b.y), c.y) - min(min(a.y, b.y), c.y) > 511) return;
    if (area < 0) { ps1_vtx t = b; b = c; c = t; area = -area; }
    device long *grad = reinterpret_cast<device long *>(scratch); gradients(a, b, c, area, grad);
    int4 box = bounds(o); ushort flat = rgb15(0, 0, a.r, a.g, a.b, false);
    for (int y = box.y; y <= box.w; y++) for (int x = box.x; x <= box.z; x++) {
        if (!inside(a, b, c, x, y, 1)) continue;
        int r = a.r, g = a.g, bl = a.b;
        if (o.flags & RENDER_GOURAUD) { r = interpolate(a.r, a, x, y, 1, grad, 0); g = interpolate(a.g, a, x, y, 1, grad, 1); bl = interpolate(a.b, a, x, y, 1, grad, 2); }
        if (o.flags & RENDER_TEXTURE) {
            int u = interpolate(a.u, a, x, y, 1, grad, 3), v = interpolate(a.v, a, x, y, 1, grad, 4); ushort sample = texel(vram, cache, u, v, o);
            if (scale > 1) save_sample(scratch, vram, x, y, u, v, sample, o);
            textured_plot(vram, x, y, 1, sample, r, g, bl, o);
        } else plot(vram, x, y, 1, o.flags & RENDER_GOURAUD ? rgb15(x, y, r, g, bl, o.flags & RENDER_DITHER) : flat, o.flags & RENDER_SEMI, o.mask, o.texpage);
    }
}
static void draw_rectangle(device ushort *vram, device ps1_render_cache &cache, constant ps1_render_op &o, device uint *scratch, uint scale) {
    int4 box = bounds(o); ushort color = rgb15(0, 0, o.a.r, o.a.g, o.a.b, false);
    for (int y = box.y; y <= box.w; y++) for (int x = box.x; x <= box.z; x++) {
        if (o.flags & RENDER_TEXTURE) {
            int u = o.a.u + (o.texpage & 0x1000 ? o.a.x - x + 1 : x - o.a.x), v = o.a.v + (o.texpage & 0x2000 ? o.a.y - y : y - o.a.y);
            ushort sample = texel(vram, cache, u, v, o); if (scale > 1) save_sample(scratch, vram, x, y, u, v, sample, o);
            textured_plot(vram, x, y, 1, sample, o.a.r, o.a.g, o.a.b, o);
        } else plot(vram, x, y, 1, color, o.flags & RENDER_SEMI, o.mask, o.texpage);
    }
}
static void draw_line(device ushort *vram, constant ps1_render_op &o) {
    int dx = o.b.x - o.a.x, dy = o.b.y - o.a.y, n = max(abs(dx), abs(dy)); if (abs(dx) > 1023 || abs(dy) > 511) return;
    bool xmajor = abs(dx) >= abs(dy); int d = xmajor ? dy : dx, sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    for (int i = 0; i <= n; i++) {
        int off = n ? int((2 * long(i) * abs(d) + n - !xmajor) / (2 * n)) : 0;
        int x = xmajor ? o.a.x + sx * i : o.a.x + sx * off, y = xmajor ? o.a.y + sy * off : o.a.y + sy * i;
        if (x < o.ax1 || x > o.ax2 || y < o.ay1 || y > o.ay2) continue;
        int r = o.a.r, g = o.a.g, b = o.a.b;
        if ((o.flags & RENDER_GOURAUD) && n) {
            r = int((long(r) * 4096 + i * (long(o.b.r - r) * 4096 / n) + 2048) >> 12);
            g = int((long(g) * 4096 + i * (long(o.b.g - g) * 4096 / n) + 2048) >> 12);
            b = int((long(b) * 4096 + i * (long(o.b.b - b) * 4096 / n) + 2048) >> 12);
        }
        plot(vram, x, y, 1, rgb15(x, y, r, g, b, o.flags & RENDER_DITHER), o.flags & RENDER_SEMI, o.mask, o.texpage);
    }
}
static void copy_rectangle(device ushort *vram, device ushort *hires, constant ps1_render_op &o, uint scale, device uint *scratch) {
    int sx = o.p[0] & 1023, sy = o.p[0] >> 16 & 511, dx = o.p[1] & 1023, dy = o.p[1] >> 16 & 511;
    int width = ((o.p[2] - 1) & 1023) + 1, height = (((o.p[2] >> 16) - 1) & 511) + 1;
    // Snapshot one source row, not the rectangle: a downward overlapping copy intentionally sees rows written by the preceding iteration (software reference).
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) scratch[x] = vram[index_at(sx + x, sy + y, 1)];
        for (int x = 0; x < width; x++) {
            device ushort &p = vram[index_at(dx + x, dy + y, 1)]; if (!((o.mask & 2) && (p & 0x8000))) p = ushort(scratch[x]) | ushort((o.mask & 1) << 15);
        }
        if (scale > 1) for (uint fy = 0; fy < scale; fy++) {
            for (int x = 0; x < width * int(scale); x++) scratch[1024 + x] = hires[index_at(sx * int(scale) + x, (sy + y) * int(scale) + int(fy), scale)];
            for (int x = 0; x < width * int(scale); x++) {
                device ushort &p = hires[index_at(dx * int(scale) + x, (dy + y) * int(scale) + int(fy), scale)];
                if (!((o.mask & 2) && (p & 0x8000))) p = ushort(scratch[1024 + x]) | ushort((o.mask & 1) << 15);
            }
        }
    }
}
static void upload_pixel(device ushort *vram, device ushort *hires, constant ps1_render_op &o, uint scale) {
    uint at = o.p[0] & 0x7ffff; device ushort &p = vram[at];
    if (!((o.mask & 2) && (p & 0x8000))) p = ushort(o.p[1]) | ushort((o.mask & 1) << 15);
    if (scale > 1) {
        int x = int(at & 1023), y = int(at >> 10);
        for (uint fy = 0; fy < scale; fy++) for (uint fx = 0; fx < scale; fx++) {
            device ushort &q = hires[index_at(x * int(scale) + int(fx), y * int(scale) + int(fy), scale)];
            if (!((o.mask & 2) && (q & 0x8000))) q = ushort(o.p[1]) | ushort((o.mask & 1) << 15);
        }
    }
}

kernel void render_native(device ushort *vram [[buffer(0)]], device ushort *hires [[buffer(1)]], device ps1_render_cache &cache [[buffer(2)]],
                          constant ps1_render_op &o [[buffer(3)]], constant uint &scale [[buffer(4)]], device uint *scratch [[buffer(5)]], uint tid [[thread_position_in_grid]]) {
    if (tid) return;
    switch (o.type) {
    case RENDER_TRIANGLE: draw_triangle(vram, cache, o, scratch, scale); break;
    case RENDER_RECT: draw_rectangle(vram, cache, o, scratch, scale); break;
    case RENDER_LINE: draw_line(vram, o); break;
    case RENDER_FILL: {
        int x0 = o.p[1] & 0x3f0, y0 = o.p[1] >> 16 & 511, width = ((o.p[2] & 1023) + 15) & ~15u, height = o.p[2] >> 16 & 511;
        ushort color = rgb15(0, 0, o.p[0] & 255, o.p[0] >> 8 & 255, o.p[0] >> 16 & 255, false);
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) vram[index_at(x0 + x, y0 + y, 1)] = color;
        break;
    }
    case RENDER_COPY: copy_rectangle(vram, hires, o, scale, scratch); break;
    case RENDER_UPLOAD: upload_pixel(vram, hires, o, scale); break;
    case RENDER_CLEAR:
        if (!o.p[0]) for (uint i = 0; i < 256; i++) cache.textag[i] = 0;
        cache.clut_valid = 0; break;
    case RENDER_RESET:
        for (uint i = 0; i < 256; i++) { for (uint j = 0; j < 4; j++) cache.texcache[i][j] = 0; cache.textag[i] = 0; cache.clutcache[i] = 0; }
        cache.clut_id = 0; cache.clut_valid = cache.clut_n = 0; break;
    }
}

kernel void render_upload(device ushort *vram [[buffer(0)]], device ushort *hires [[buffer(1)]], device ps1_render_cache &cache [[buffer(2)]],
                          constant ps1_render_op *ops [[buffer(3)]], constant uint &scale [[buffer(4)]], device uint *scratch [[buffer(5)]],
                          constant uint &count [[buffer(6)]], uint i [[thread_position_in_grid]]) {
    (void)cache; (void)scratch; if (i < count) upload_pixel(vram, hires, ops[i], scale);
}

// The measured title replay spent all 4,403 samples waiting for GPU completion while fills and flat rectangles ran in one lane. Their pixels have no texture/cache
// dependencies and unique destinations within a command; dispatch them in parallel while preserving the barriers between commands (psx-spx, Quick Rectangle Fill).
kernel void render_flat(device ushort *vram [[buffer(0)]], device ushort *hires [[buffer(1)]], device ps1_render_cache &cache [[buffer(2)]],
                        constant ps1_render_op &o [[buffer(3)]], constant uint &scale [[buffer(4)]], device uint *scratch [[buffer(5)]], uint2 gid [[thread_position_in_grid]]) {
    (void)cache; (void)scratch;
    if (o.type == RENDER_FILL) {
        uint width = ((o.p[2] & 1023) + 15) & ~15u, height = o.p[2] >> 16 & 511;
        if (gid.x >= width || gid.y >= height) return;
        int x = int(o.p[1] & 0x3f0) + int(gid.x), y = int(o.p[1] >> 16 & 511) + int(gid.y);
        ushort color = rgb15(0, 0, o.p[0] & 255, o.p[0] >> 8 & 255, o.p[0] >> 16 & 255, false);
        vram[index_at(x, y, 1)] = color;
        if (scale > 1) for (uint fy = 0; fy < scale; fy++) for (uint fx = 0; fx < scale; fx++) hires[index_at(x * int(scale) + int(fx), y * int(scale) + int(fy), scale)] = color;
    } else if (o.type == RENDER_RECT && !(o.flags & RENDER_TEXTURE)) {
        int4 box = bounds(o); if (box.x > box.z || box.y > box.w || gid.x >= uint(box.z - box.x + 1) || gid.y >= uint(box.w - box.y + 1)) return;
        int x = box.x + int(gid.x), y = box.y + int(gid.y); ushort color = rgb15(0, 0, o.a.r, o.a.g, o.a.b, false);
        plot(vram, x, y, 1, color, o.flags & RENDER_SEMI, o.mask, o.texpage);
        if (scale > 1) for (uint fy = 0; fy < scale; fy++) for (uint fx = 0; fx < scale; fx++) plot(hires, x * int(scale) + int(fx), y * int(scale) + int(fy), scale, color, o.flags & RENDER_SEMI, o.mask, o.texpage);
    }
}

// The title replay remains 46.65 s against the 18.52 s software baseline after parallel flat fills: it sends about 280 rectangle/fill/copy operations a frame.
// For a <=16x16 sprite with no texture window, each requested cache slot has only one tag: horizontal slot periods span at least 32 UV samples and vertical periods
// at least 32 rows, while the UV wrap period is 256, a multiple of each cache period. Flips retain that bound. The host excludes the full source page and CLUT from
// the destination, so all loads see stable VRAM, including the four-word cache blocks. Larger/windowed/feedback draws keep the ordered native path.
// One owner loads each tag, then a group barrier makes the immutable texture/CLUT samples available to parallel pixels. Groups execute consecutive eligible sprites
// in order, avoiding a command-encoder dispatch per tiny primitive while retaining stale matching cache entries and all native masks/blends.
kernel void render_sprites(device ushort *vram [[buffer(0)]], device ushort *hires [[buffer(1)]], device ps1_render_cache &cache [[buffer(2)]],
                           constant ps1_render_op *ops [[buffer(3)]], constant uint &scale [[buffer(4)]], device uint *scratch [[buffer(5)]],
                           constant uint &count [[buffer(6)]], uint tid [[thread_index_in_threadgroup]]) {
    (void)scratch; threadgroup uint reload_clut;
    for (uint n = 0; n < count; n++) {
        constant ps1_render_op &o = ops[n]; int4 box = bounds(o);
        uint width = uint(box.z - box.x + 1), height = uint(box.w - box.y + 1), pixels = width * height;
        uint depth = o.texpage >> 7 & 3, need = depth == 0 ? 16 : depth == 1 ? 256 : 0, clut = o.clut & 0x7fff;
        if (!tid) reload_clut = need && (!cache.clut_valid || cache.clut_id != clut || uint(cache.clut_n) < need);
        threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
        int x = box.x + int(tid % width), y = box.y + int(tid / width), u = 0, v = 0;
        if (tid < pixels) {
            u = o.a.u + (o.texpage & 0x1000 ? o.a.x - x + 1 : x - o.a.x); v = o.a.v + (o.texpage & 0x2000 ? o.a.y - y : y - o.a.y);
            uint4 a = texture_address(u, v, o.texpage, 0); uint tag = a.y * 1024 + (a.x & ~3u);
            bool owner = tid % width == 0;
            if (!owner) {
                int previous_u = o.a.u + (o.texpage & 0x1000 ? o.a.x - (x - 1) + 1 : (x - 1) - o.a.x);
                uint4 previous = texture_address(previous_u, v, o.texpage, 0); owner = previous.y * 1024 + (previous.x & ~3u) != tag;
            }
            if (owner && cache.textag[a.z] != tag + 1) {
                for (uint i = 0; i < 4; i++) cache.texcache[a.z][i] = vram[tag + i]; cache.textag[a.z] = tag + 1;
            }
        }
        if (reload_clut && tid < need) cache.clutcache[tid] = vram[(clut >> 6) * 1024 + (((clut & 63) * 16 + tid) & 1023)];
        if (reload_clut && !tid) { cache.clut_id = ushort(clut); cache.clut_valid = 1; cache.clut_n = int(need); }
        threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
        if (tid < pixels) {
            ushort sample = texel_extra(vram, cache, u, v, o);
            textured_plot(vram, x, y, 1, sample, o.a.r, o.a.g, o.a.b, o);
            if (scale > 1) for (uint fy = 0; fy < scale; fy++) for (uint fx = 0; fx < scale; fx++)
                textured_plot(hires, x * int(scale) + int(fx), y * int(scale) + int(fy), scale, sample, o.a.r, o.a.g, o.a.b, o);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
    }
}

kernel void render_scaled(device const ushort *vram [[buffer(0)]], device ushort *hires [[buffer(1)]], device const ps1_render_cache &cache [[buffer(2)]],
                          constant ps1_render_op &o [[buffer(3)]], constant uint &scale [[buffer(4)]], device uint *scratch [[buffer(5)]], uint2 gid [[thread_position_in_grid]]) {
    if (scale <= 1) return;
    if (o.type == RENDER_FILL) {
        uint width = ((o.p[2] & 1023) + 15) & ~15u, height = o.p[2] >> 16 & 511;
        if (gid.x >= width * scale || gid.y >= height * scale) return;
        int x = int(o.p[1] & 0x3f0) * int(scale) + int(gid.x), y = int(o.p[1] >> 16 & 511) * int(scale) + int(gid.y);
        hires[index_at(x, y, scale)] = rgb15(0, 0, o.p[0] & 255, o.p[0] >> 8 & 255, o.p[0] >> 16 & 255, false); return;
    }
    int4 box = bounds(o); if (box.x > box.z || box.y > box.w) return;
    if (gid.x >= uint(box.z - box.x + 1) * scale || gid.y >= uint(box.w - box.y + 1) * scale) return;
    int x = box.x * int(scale) + int(gid.x), y = box.y * int(scale) + int(gid.y), nx = x / int(scale), ny = y / int(scale);
    int r = o.a.r, g = o.a.g, bl = o.a.b, u = o.a.u, v = o.a.v; bool native_covered = true;
    bool feedback = (o.flags & RENDER_TEXTURE) && texture_feedback(o, box);
    if (o.type == RENDER_TRIANGLE) {
        ps1_vtx a = o.a, b = o.b, c = o.c; long area = edge(a, b, c.x, c.y, 1);
        if (!area || max(max(a.x, b.x), c.x) - min(min(a.x, b.x), c.x) > 1023 || max(max(a.y, b.y), c.y) - min(min(a.y, b.y), c.y) > 511) return;
        if (area < 0) { ps1_vtx t = b; b = c; c = t; }
        native_covered = inside(a, b, c, nx, ny, 1);
        if (feedback) { if (native_covered && sample_written(scratch, nx, ny)) hires[index_at(x, y, scale)] = vram[index_at(nx, ny, 1)]; return; }
        if (!inside(a, b, c, x, y, scale)) return;
        device const long *grad = reinterpret_cast<device const long *>(scratch);
        if (o.flags & RENDER_GOURAUD) { r = interpolate(a.r, a, x, y, scale, grad, 0); g = interpolate(a.g, a, x, y, scale, grad, 1); bl = interpolate(a.b, a, x, y, scale, grad, 2); }
        if (o.flags & RENDER_TEXTURE) { u = interpolate(a.u, a, x, y, scale, grad, 3); v = interpolate(a.v, a, x, y, scale, grad, 4); }
    } else if (o.type == RENDER_RECT) {
        // Sprites keep their native texture pixel grid, including flips; this is identical to the batched sprite path. Polygons above interpolate genuine subpixel UVs.
        u = o.a.u + (o.texpage & 0x1000 ? o.a.x - nx + 1 : nx - o.a.x); v = o.a.v + (o.texpage & 0x2000 ? o.a.y - ny : ny - o.a.y);
    } else if (o.type == RENDER_LINE) {
        // Preserve native line coverage as a one-console-pixel strip. Shade each fine pixel once, so blending and masks keep existing subpixel geometry underneath.
        int dx = o.b.x - o.a.x, dy = o.b.y - o.a.y, n = max(abs(dx), abs(dy)); if (abs(dx) > 1023 || abs(dy) > 511) return;
        bool xmajor = abs(dx) >= abs(dy); int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
        int i = xmajor ? (nx - o.a.x) * sx : (ny - o.a.y) * sy; if (i < 0 || i > n) return;
        int d = xmajor ? dy : dx, off = n ? int((2 * long(i) * abs(d) + n - !xmajor) / (2 * n)) : 0;
        if (nx != (xmajor ? o.a.x + sx * i : o.a.x + sx * off) || ny != (xmajor ? o.a.y + sy * off : o.a.y + sy * i)) return;
        if ((o.flags & RENDER_GOURAUD) && n) {
            r = int((long(r) * 4096 + i * (long(o.b.r - r) * 4096 / n) + 2048) >> 12);
            g = int((long(g) * 4096 + i * (long(o.b.g - g) * 4096 / n) + 2048) >> 12);
            bl = int((long(bl) * 4096 + i * (long(o.b.b - bl) * 4096 / n) + 2048) >> 12);
        }
        plot(hires, x, y, scale, rgb15(nx, ny, r, g, bl, o.flags & RENDER_DITHER), o.flags & RENDER_SEMI, o.mask, o.texpage); return;
    } else return;
    if (o.flags & RENDER_TEXTURE) {
        // Feedback draws read pixels being written in raster order. They have no independent fine-resolution source; retain their exact native result as pixel blocks.
        // Conservative page/CLUT overlap detection also covers texture-window aliases, avoiding a false claim of independent subpixel samples in these commands.
        if (feedback) { if (native_covered && sample_written(scratch, nx, ny)) hires[index_at(x, y, scale)] = vram[index_at(nx, ny, 1)]; return; }
        uint raw = scratch[SAMPLE_BASE + index_at(nx, ny, 1)]; int2 uv = window_uv(u, v, o.texwin); uint key = uint(uv.x) | uint(uv.y) << 8;
        ushort sample = native_covered && (raw >> 16) == key ? ushort(raw) : texel_extra(vram, cache, u, v, o);
        textured_plot(hires, x, y, scale, sample, r, g, bl, o);
    } else plot(hires, x, y, scale, rgb15(nx, ny, r, g, bl, o.flags & RENDER_DITHER), o.flags & RENDER_SEMI, o.mask, o.texpage);
}
