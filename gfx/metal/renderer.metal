// Integer GPU rendering (plan.md, phase 6), from our MIT software reference and public psx-spx GPU documentation. Native coverage/interpolation deliberately uses
// the reference's integer edges and 12-bit gradients: ordinary floating-point triangle rasterization does not reproduce its VRAM. Texture/cache mutations and
// overlapping copies run in command/scanline order. A separate parallel pass samples geometry at subpixels for 2x/4x; CPU-visible VRAM always stays at native size.
#include "../../runtime/render.h"

constant uint SAMPLE_BASE = 32;
constant uint WRITTEN_BASE = SAMPLE_BASE + 1024 * 512;
constant uint CACHE_EVENT_BASE = WRITTEN_BASE + 1024 * 512 / 32;
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
template<typename Cache>
static ushort texel_extra(device const ushort *vram, Cache cache, int u, int v, constant ps1_render_op &o) {
    uint4 a = texture_address(u, v, o.texpage, o.texwin); uint tag = a.y * 1024 + (a.x & ~3u);
    ushort c = cache->textag[a.z] == tag + 1 ? cache->texcache[a.z][a.x & 3] : vram[a.y * 1024 + a.x];
    if (!a.w) return c;
    int2 uv = window_uv(u, v, o.texwin); uint entry = a.w == 2 ? c >> ((uv.x & 3) * 4) & 15 : c >> ((uv.x & 1) * 8) & 255;
    uint clut = o.clut & 0x7fff, need = a.w == 2 ? 16 : 256;
    if (cache->clut_valid && cache->clut_id == clut && uint(cache->clut_n) >= need) return cache->clutcache[entry];
    return vram[(clut >> 6) * 1024 + (((clut & 63) * 16 + entry) & 1023)];
}
static void textured_plot(device ushort *vram, int x, int y, uint scale, ushort c, int r, int g, int b, constant ps1_render_op &o) {
    if (!c) return;
    ushort mask = c & 0x8000;
    if (!(o.flags & RENDER_RAW)) c = rgb15(x / int(scale), y / int(scale), clamp8((c & 31) * r >> 4), clamp8((c >> 5 & 31) * g >> 4), clamp8((c >> 10 & 31) * b >> 4), o.flags & RENDER_DITHER) | mask;
    plot(vram, x, y, scale, c, (o.flags & RENDER_SEMI) && mask, o.mask, o.texpage);
}
// GP0 positions and drawing offsets are signed 11-bit values (psx-spx, Vertex attributes / Drawing Offset). Their sums, the 1x/2x/4x raster coordinates and
// even a rejected polygon's edge products fit signed 32 bits. GT2's close-up replay spent up to ~62 ms in the GPU with widened per-sample arithmetic.
static int edge(ps1_vtx a, ps1_vtx b, int x, int y, uint scale) {
    return (b.x - a.x) * (y - a.y * int(scale)) - (b.y - a.y) * (x - a.x * int(scale));
}
static bool owns(ps1_vtx a, ps1_vtx b) { return b.y < a.y || (b.y == a.y && b.x > a.x); }
static bool inside(ps1_vtx a, ps1_vtx b, ps1_vtx c, int x, int y, uint scale) {
    int wa = edge(b, c, x, y, scale), wb = edge(c, a, x, y, scale), wc = edge(a, b, x, y, scale);
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
static void gradients(constant ps1_render_op &o, device long *grad, constant long *prepared) {
    for (uint i = 0; i < 10; i++) grad[i] = prepared[o.p[3] * 10 + i];
}
template<typename Grad>
static int interpolate(int value, ps1_vtx a, int x, int y, uint scale, Grad grad, uint i) {
    // Called only after coverage/size rejection. The exact affine value lies within the 8-bit vertex attributes; truncating the two 12-bit gradients adds
    // <(1023+511)/4096 error. Thus the FINAL numerator fits 32 bits at scale<=4, even when skinny triangles' products exceed that range before cancellation.
    // Unsigned products/addition preserve its low 32 bits exactly, then bit-casting recovers the signed numerator before the reference's rounding shift.
    // The determinant-one regression checks cancellation of large products in both fine colors and UVs.
    uint n = uint(value) * 4096u * scale + uint(grad[i]) * uint(x - a.x * int(scale)) + uint(grad[i + 5]) * uint(y - a.y * int(scale)) + 2048u * scale;
    return as_type<int>(n) >> (12 + log_scale(scale));
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
static void draw_triangle(device ushort *vram, device ps1_render_cache &cache, constant ps1_render_op &o, device uint *scratch, uint scale, constant long *prepared) {
    ps1_vtx a = o.a, b = o.b, c = o.c; long area = edge(a, b, c.x, c.y, 1);
    if (!area || max(max(a.x, b.x), c.x) - min(min(a.x, b.x), c.x) > 1023 || max(max(a.y, b.y), c.y) - min(min(a.y, b.y), c.y) > 511) return;
    if (area < 0) { ps1_vtx t = b; b = c; c = t; area = -area; }
    device long *grad = reinterpret_cast<device long *>(scratch); gradients(o, grad, prepared);
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
                          constant ps1_render_op &o [[buffer(3)]], constant uint &scale [[buffer(4)]], device uint *scratch [[buffer(5)]], constant long *prepared [[buffer(7)]], uint tid [[thread_position_in_grid]]) {
    if (tid) return;
    switch (o.type) {
    case RENDER_TRIANGLE: draw_triangle(vram, cache, o, scratch, scale, prepared); break;
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

// The host proves that source and destination are disjoint (or identical). Copy the fine samples themselves, preserving geometry, blends and per-sample masks;
// replicating native VRAM here would lose the extra detail whenever a game moves its framebuffer. Overlapping copies retain copy_rectangle's ordered rows.
kernel void render_copy(device ushort *vram [[buffer(0)]], device ushort *hires [[buffer(1)]], constant ps1_render_op &o [[buffer(3)]],
                        constant uint &scale [[buffer(4)]], uint2 gid [[thread_position_in_grid]]) {
    uint width = ((o.p[2] - 1) & 1023) + 1, height = (((o.p[2] >> 16) - 1) & 511) + 1;
    if (gid.x >= width || gid.y >= height) return;
    int sx = int(o.p[0] & 1023) + int(gid.x), sy = int(o.p[0] >> 16 & 511) + int(gid.y);
    int dx = int(o.p[1] & 1023) + int(gid.x), dy = int(o.p[1] >> 16 & 511) + int(gid.y);
    ushort source = vram[index_at(sx, sy, 1)]; device ushort &dest = vram[index_at(dx, dy, 1)];
    if (!((o.mask & 2) && (dest & 0x8000))) dest = source | ushort((o.mask & 1) << 15);
    if (scale > 1) for (uint fy = 0; fy < scale; fy++) for (uint fx = 0; fx < scale; fx++) {
        ushort fine = hires[index_at(sx * int(scale) + int(fx), sy * int(scale) + int(fy), scale)];
        device ushort &p = hires[index_at(dx * int(scale) + int(fx), dy * int(scale) + int(fy), scale)];
        if (!((o.mask & 2) && (p & 0x8000))) p = fine | ushort((o.mask & 1) << 15);
    }
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

// The OpenBIOS animation missed 16.7 ms on 145 boot frames: two large untextured triangles alone took ~64 ms in the single-lane native shader. Their pixels have
// unique destinations and no texture/cache feedback, so exact native coverage and color can run in parallel. Prepare the reference's 12-bit gradients once per
// Gouraud triangle; keep the same top-left edges, winding, size rejection and flat/Gouraud dither rules (psx-spx, GPU Render Polygon Commands / Vertex attributes).
static void triangle_prepare(constant ps1_render_op &o, device long *grad, uint tid, constant long *prepared) {
    if (tid) return;
    ps1_vtx a = o.a, b = o.b, c = o.c; long area = edge(a, b, c.x, c.y, 1);
    if (!area || max(max(a.x, b.x), c.x) - min(min(a.x, b.x), c.x) > 1023 || max(max(a.y, b.y), c.y) - min(min(a.y, b.y), c.y) > 511) return;
    if (area < 0) { ps1_vtx t = b; b = c; c = t; area = -area; }
    gradients(o, grad, prepared);
}

static void triangle(device ushort *vram, constant ps1_render_op &o,
                            device const long *grad, uint2 gid) {
    ps1_vtx a = o.a, b = o.b, c = o.c; long area = edge(a, b, c.x, c.y, 1);
    if (!area || max(max(a.x, b.x), c.x) - min(min(a.x, b.x), c.x) > 1023 || max(max(a.y, b.y), c.y) - min(min(a.y, b.y), c.y) > 511) return;
    if (area < 0) { ps1_vtx t = b; b = c; c = t; }
    int4 box = bounds(o);
    if (gid.x >= uint(box.z - box.x + 1) || gid.y >= uint(box.w - box.y + 1)) return;
    int x = box.x + int(gid.x), y = box.y + int(gid.y);
    if (!inside(a, b, c, x, y, 1)) return;
    ushort color = rgb15(0, 0, a.r, a.g, a.b, false);
    if (o.flags & RENDER_GOURAUD) color = rgb15(x, y, interpolate(a.r, a, x, y, 1, grad, 0), interpolate(a.g, a, x, y, 1, grad, 1),
                                             interpolate(a.b, a, x, y, 1, grad, 2), o.flags & RENDER_DITHER);
    plot(vram, x, y, 1, color, o.flags & RENDER_SEMI, o.mask, o.texpage);
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
            ushort sample = texel_extra(vram, &cache, u, v, o);
            textured_plot(vram, x, y, 1, sample, o.a.r, o.a.g, o.a.b, o);
            if (scale > 1) for (uint fy = 0; fy < scale; fy++) for (uint fx = 0; fx < scale; fx++)
                textured_plot(hires, x * int(scale) + int(fx), y * int(scale) + int(fy), scale, sample, o.a.r, o.a.g, o.a.b, o);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
    }
}

// Soldier selection measured ~64 ms of GPU work at 1x: sixteen large textured rectangles fell through to the serial native shader. A nonfeedback primitive has
// immutable source VRAM, but its incoming texture cache may be stale (psx-spx, GPU Texture Caching / Cache entries, and Clear Cache). For each slot, stale bytes
// survive exactly until its first requested tag different from the incoming tag. After that miss, every requested block comes from immutable VRAM, even on a
// return to the original tag. Track that first miss and the last requested tag, shade independent pixels from the original cache/VRAM, then commit final tags.
// The full mission still had 341 GPU frames over 16.7 ms, including a 45.8 ms frame with 88 textured triangles. Their covered native pixels use the same cache
// event reduction in raster order, with the reference's exact gradients and edges. Events live beyond the sample/written arrays: finer triangles retain observed
// native samples while consulting the final cache for additional UVs. Host barriers separate these four passes; feedback keeps its ordered path.
static int2 rectangle_uv(constant ps1_render_op &o, int x, int y) {
    return int2(o.a.u + (o.texpage & 0x1000 ? o.a.x - x + 1 : x - o.a.x), o.a.v + (o.texpage & 0x2000 ? o.a.y - y : y - o.a.y));
}

template<typename Grad>
static bool texture_position(constant ps1_render_op &o, Grad grad, int x, int y, thread int2 &uv) {
    if (o.type == RENDER_RECT) { uv = rectangle_uv(o, x, y); return true; }
    ps1_vtx a = o.a, b = o.b, c = o.c; long area = edge(a, b, c.x, c.y, 1);
    if (!area || max(max(a.x, b.x), c.x) - min(min(a.x, b.x), c.x) > 1023 || max(max(a.y, b.y), c.y) - min(min(a.y, b.y), c.y) > 511) return false;
    if (area < 0) { ps1_vtx t = b; b = c; c = t; }
    if (!inside(a, b, c, x, y, 1)) return false;
    uv = int2(interpolate(a.u, a, x, y, 1, grad, 3), interpolate(a.v, a, x, y, 1, grad, 4)); return true;
}

static void texture_prepare(constant ps1_render_op &o, device uint *scratch, uint tid, constant long *prepared) {
    device atomic_uint *state = reinterpret_cast<device atomic_uint *>(scratch + CACHE_EVENT_BASE);
    atomic_store_explicit(state + tid, 0xffffffffu, memory_order_relaxed);
    atomic_store_explicit(state + 256 + tid, 0u, memory_order_relaxed);
    if (!tid) {
        atomic_store_explicit(state + 512, 0u, memory_order_relaxed);
        if (o.type == RENDER_TRIANGLE) {
            ps1_vtx a = o.a, b = o.b, c = o.c; long area = edge(a, b, c.x, c.y, 1);
            if (!area || max(max(a.x, b.x), c.x) - min(min(a.x, b.x), c.x) > 1023 || max(max(a.y, b.y), c.y) - min(min(a.y, b.y), c.y) > 511) return;
            if (area < 0) { ps1_vtx t = b; b = c; c = t; area = -area; }
            gradients(o, reinterpret_cast<device long *>(scratch), prepared);
        }
    }
}

static void texture_track(device const ushort *vram, device ps1_render_cache &cache, constant ps1_render_op &o,
                                 device uint *scratch, uint2 gid) {
    int4 box = bounds(o); uint width = uint(box.z - box.x + 1);
    if (gid.x >= width || gid.y >= uint(box.w - box.y + 1)) return;
    device const long *grad = reinterpret_cast<device const long *>(scratch); int x = box.x + int(gid.x), y = box.y + int(gid.y); int2 uv;
    if (!texture_position(o, grad, x, y, uv)) return;
    uint4 a = texture_address(uv.x, uv.y, o.texpage, o.texwin); uint tag = a.y * 1024 + (a.x & ~3u);
    // Repeated covered hits within one horizontal cache-line run cannot change either event. A triangle's first covered pixel may follow an uncovered neighbor.
    if (gid.x) {
        int2 previous_uv;
        if (texture_position(o, grad, x - 1, y, previous_uv)) {
            uint4 previous = texture_address(previous_uv.x, previous_uv.y, o.texpage, o.texwin);
            if (previous.y * 1024 + (previous.x & ~3u) == tag) return;
        }
    }
    device atomic_uint *state = reinterpret_cast<device atomic_uint *>(scratch + CACHE_EVENT_BASE); uint at = gid.y * width + gid.x;
    if (cache.textag[a.z] != tag + 1) atomic_fetch_min_explicit(state + a.z, at, memory_order_relaxed);
    atomic_fetch_max_explicit(state + 256 + a.z, at + 1, memory_order_relaxed);
    // A nonempty bounding box need not contain a native triangle sample. Only the first actual request loads the palette; no reader observes it until draw.
    // Metadata stays unchanged until finish, so all lanes see the same incoming CLUT identity and a zero-coverage primitive changes no cache bytes.
    if (!atomic_fetch_or_explicit(state + 512, 1u, memory_order_relaxed)) {
        uint depth = o.texpage >> 7 & 3, need = depth == 0 ? 16 : depth == 1 ? 256 : 0, clut = o.clut & 0x7fff;
        if (need && (!cache.clut_valid || cache.clut_id != clut || uint(cache.clut_n) < need))
            for (uint i = 0; i < need; i++) cache.clutcache[i] = vram[(clut >> 6) * 1024 + (((clut & 63) * 16 + i) & 1023)];
    }
}

static void texture_draw(device ushort *vram, device ushort *hires, device const ps1_render_cache &cache,
                                constant ps1_render_op &o, constant uint &scale, device uint *scratch, uint2 gid) {
    int4 box = bounds(o); uint width = uint(box.z - box.x + 1);
    if (gid.x >= width || gid.y >= uint(box.w - box.y + 1)) return;
    device const long *grad = reinterpret_cast<device const long *>(scratch); int x = box.x + int(gid.x), y = box.y + int(gid.y); int2 uv;
    if (!texture_position(o, grad, x, y, uv)) return;
    uint4 a = texture_address(uv.x, uv.y, o.texpage, o.texwin); uint tag = a.y * 1024 + (a.x & ~3u);
    device const uint *state = scratch + CACHE_EVENT_BASE;
    ushort sample = cache.textag[a.z] == tag + 1 && gid.y * width + gid.x < state[a.z] ? cache.texcache[a.z][a.x & 3] : vram[a.y * 1024 + a.x];
    if (a.w) {
        int2 mapped = window_uv(uv.x, uv.y, o.texwin);
        uint entry = a.w == 2 ? sample >> ((mapped.x & 3) * 4) & 15 : sample >> ((mapped.x & 1) * 8) & 255;
        sample = cache.clutcache[entry];
    }
    int r = o.a.r, g = o.a.g, b = o.a.b;
    if (o.type == RENDER_TRIANGLE) {
        if (o.flags & RENDER_GOURAUD) { r = interpolate(r, o.a, x, y, 1, grad, 0); g = interpolate(g, o.a, x, y, 1, grad, 1); b = interpolate(b, o.a, x, y, 1, grad, 2); }
        if (scale > 1) {
            int2 mapped = window_uv(uv.x, uv.y, o.texwin);
            scratch[SAMPLE_BASE + index_at(x, y, 1)] = uint(sample) | uint(mapped.x) << 16 | uint(mapped.y) << 24;
        }
    }
    textured_plot(vram, x, y, 1, sample, r, g, b, o);
    if (o.type == RENDER_RECT && scale > 1) for (uint fy = 0; fy < scale; fy++) for (uint fx = 0; fx < scale; fx++)
        textured_plot(hires, x * int(scale) + int(fx), y * int(scale) + int(fy), scale, sample, o.a.r, o.a.g, o.a.b, o);
}

static void texture_finish(device const ushort *vram, device ps1_render_cache &cache,
                                  constant ps1_render_op &o, device const uint *scratch, uint tid) {
    device const uint *state = scratch + CACHE_EVENT_BASE;
    if (state[256 + tid] && state[tid] != 0xffffffffu) {
        int4 box = bounds(o); uint width = uint(box.z - box.x + 1), at = state[256 + tid] - 1;
        int2 uv; texture_position(o, reinterpret_cast<device const long *>(scratch), box.x + int(at % width), box.y + int(at / width), uv);
        uint4 a = texture_address(uv.x, uv.y, o.texpage, o.texwin); uint tag = a.y * 1024 + (a.x & ~3u);
        for (uint i = 0; i < 4; i++) cache.texcache[tid][i] = vram[tag + i]; cache.textag[tid] = tag + 1;
    }
    if (!tid && state[512]) {
        uint depth = o.texpage >> 7 & 3, need = depth == 0 ? 16 : depth == 1 ? 256 : 0, clut = o.clut & 0x7fff;
        if (need && (!cache.clut_valid || cache.clut_id != clut || uint(cache.clut_n) < need)) {
            cache.clut_id = ushort(clut); cache.clut_valid = 1; cache.clut_n = int(need);
        }
    }
}

static void scaled(device const ushort *vram, device ushort *hires, device const ps1_render_cache &cache,
                          constant ps1_render_op &o, constant uint &scale, device uint *scratch, uint2 gid) {
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
    bool feedback = o.flags & RENDER_FEEDBACK;
    if (o.type == RENDER_TRIANGLE) {
        ps1_vtx a = o.a, b = o.b, c = o.c; long area = edge(a, b, c.x, c.y, 1);
        if (!area || max(max(a.x, b.x), c.x) - min(min(a.x, b.x), c.x) > 1023 || max(max(a.y, b.y), c.y) - min(min(a.y, b.y), c.y) > 511) return;
        if (area < 0) { ps1_vtx t = b; b = c; c = t; }
        native_covered = inside(a, b, c, nx, ny, 1);
        if (feedback) { if (native_covered && sample_written(scratch, nx, ny)) hires[index_at(x, y, scale)] = vram[index_at(nx, ny, 1)]; return; }
        if (!inside(a, b, c, x, y, scale)) return;
        device const long *grad = reinterpret_cast<device const long *>(scratch);
        if (o.flags & RENDER_GOURAUD) { r = interpolate(a.r, a, x, y, scale, grad, 0); g = interpolate(a.g, a, x, y, scale, grad, 1); bl = interpolate(a.b, a, x, y, scale, grad, 2); }
        if (o.flags & RENDER_TEXTURE) {
            u = interpolate(a.u, a, x, y, scale, grad, 3); v = interpolate(a.v, a, x, y, scale, grad, 4);
            if (o.flags & RENDER_UV_LIMIT) { u = clamp(u, int(o.p[0] & 0xffff), int(o.p[0] >> 16)); v = clamp(v, int(o.p[1] & 0xffff), int(o.p[1] >> 16)); }
        }
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
        ushort sample = native_covered && (raw >> 16) == key ? ushort(raw) : texel_extra(vram, &cache, u, v, o);
        textured_plot(hires, x, y, scale, sample, r, g, bl, o);
    } else plot(hires, x, y, scale, rgb15(nx, ny, r, g, bl, o.flags & RENDER_DITHER), o.flags & RENDER_SEMI, o.mask, o.texpage);
}

// Keep the independently tested per-primitive entry points for large draws. Small ordered batches call the same pixel/cache routines within one workgroup.
kernel void render_triangle_prepare(constant ps1_render_op &o [[buffer(3)]], device long *grad [[buffer(5)]], constant long *prepared [[buffer(7)]], uint tid [[thread_position_in_grid]]) {
    triangle_prepare(o, grad, tid, prepared);
}

kernel void render_triangle(device ushort *vram [[buffer(0)]], constant ps1_render_op &o [[buffer(3)]],
                            device const long *grad [[buffer(5)]], uint2 gid [[thread_position_in_grid]]) {
    triangle(vram, o, grad, gid);
}

kernel void render_texture_prepare(constant ps1_render_op &o [[buffer(3)]], device uint *scratch [[buffer(5)]], constant long *prepared [[buffer(7)]], uint tid [[thread_position_in_grid]]) {
    texture_prepare(o, scratch, tid, prepared);
}

kernel void render_texture_track(device const ushort *vram [[buffer(0)]], device ps1_render_cache &cache [[buffer(2)]], constant ps1_render_op &o [[buffer(3)]],
                                 device uint *scratch [[buffer(5)]], uint2 gid [[thread_position_in_grid]]) {
    texture_track(vram, cache, o, scratch, gid);
}

kernel void render_texture_draw(device ushort *vram [[buffer(0)]], device ushort *hires [[buffer(1)]], device const ps1_render_cache &cache [[buffer(2)]],
                                constant ps1_render_op &o [[buffer(3)]], constant uint &scale [[buffer(4)]], device uint *scratch [[buffer(5)]], uint2 gid [[thread_position_in_grid]]) {
    texture_draw(vram, hires, cache, o, scale, scratch, gid);
}

kernel void render_texture_finish(device const ushort *vram [[buffer(0)]], device ps1_render_cache &cache [[buffer(2)]],
                                  constant ps1_render_op &o [[buffer(3)]], device const uint *scratch [[buffer(5)]], uint tid [[thread_position_in_grid]]) {
    texture_finish(vram, cache, o, scratch, tid);
}

kernel void render_scaled(device const ushort *vram [[buffer(0)]], device ushort *hires [[buffer(1)]], device const ps1_render_cache &cache [[buffer(2)]],
                          constant ps1_render_op &o [[buffer(3)]], constant uint &scale [[buffer(4)]], device uint *scratch [[buffer(5)]], uint2 gid [[thread_position_in_grid]]) {
    scaled(vram, hires, cache, o, scale, scratch, gid);
}

// GT2's Midfield replay draws ~3,300 triangles in a busy field: the old small-triangle batch took ~34 ms at 4x. Five device-memory barriers per textured
// primitive serialized cache/event/sample traffic. This one workgroup owns a cache snapshot and per-primitive events/samples in threadgroup memory instead.
// Independent textures cannot read their own destination or CLUT writes (the host's alias proof). Only the barrier between primitives needs device visibility;
// it preserves later source reads, blending and masks. Cache state is copied back before the next encoder operation. Native-empty draws leave it unchanged.
kernel void render_triangles(device ushort *vram [[buffer(0)]], device ushort *hires [[buffer(1)]], device ps1_render_cache &cache [[buffer(2)]],
                             constant ps1_render_op *ops [[buffer(3)]], constant uint &scale [[buffer(4)]], device uint *scratch [[buffer(5)]],
                             constant uint &count [[buffer(6)]], constant long *prepared [[buffer(7)]], uint tid [[thread_index_in_threadgroup]]) {
    (void)scratch;
    threadgroup ps1_render_cache local;
    threadgroup atomic_uint first[256], last[256], active;
    threadgroup uint samples[256], need_clut;
    for (uint k = 0; k < 4; k++) local.texcache[tid][k] = cache.texcache[tid][k];
    local.textag[tid] = cache.textag[tid]; local.clutcache[tid] = cache.clutcache[tid];
    if (!tid) { local.clut_id = cache.clut_id; local.clut_valid = cache.clut_valid; local.clut_n = cache.clut_n; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint n = 0; n < count; n++) {
        constant ps1_render_op &o = ops[n]; int4 box = bounds(o);
        uint width = uint(box.z - box.x + 1), height = uint(box.w - box.y + 1), pixels = width * height;
        constant long *grad = prepared + o.p[3] * 10;
        ps1_vtx a = o.a, b = o.b, c = o.c; long area = edge(a, b, c.x, c.y, 1);
        if (!area || max(max(a.x, b.x), c.x) - min(min(a.x, b.x), c.x) > 1023 || max(max(a.y, b.y), c.y) - min(min(a.y, b.y), c.y) > 511) continue;
        if (area < 0) { ps1_vtx t = b; b = c; c = t; }
        if (o.flags & RENDER_TEXTURE) {
            samples[tid] = 0; atomic_store_explicit(first + tid, 0xffffffffu, memory_order_relaxed); atomic_store_explicit(last + tid, 0u, memory_order_relaxed);
            if (!tid) atomic_store_explicit(&active, 0u, memory_order_relaxed);
            threadgroup_barrier(mem_flags::mem_threadgroup);
            for (uint i = tid; i < pixels; i += 256) {
                int x = box.x + int(i % width), y = box.y + int(i / width); int2 uv;
                if (!texture_position(o, grad, x, y, uv)) continue;
                uint4 address = texture_address(uv.x, uv.y, o.texpage, o.texwin); uint tag = address.y * 1024 + (address.x & ~3u);
                if (local.textag[address.z] != tag + 1) atomic_fetch_min_explicit(first + address.z, i, memory_order_relaxed);
                atomic_fetch_max_explicit(last + address.z, i + 1, memory_order_relaxed);
                atomic_store_explicit(&active, 1u, memory_order_relaxed);
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            uint depth = o.texpage >> 7 & 3, need = depth == 0 ? 16 : depth == 1 ? 256 : 0, clut = o.clut & 0x7fff;
            if (!tid) need_clut = atomic_load_explicit(&active, memory_order_relaxed) && need && (!local.clut_valid || local.clut_id != clut || uint(local.clut_n) < need);
            threadgroup_barrier(mem_flags::mem_threadgroup);
            if (need_clut && tid < need) local.clutcache[tid] = vram[(clut >> 6) * 1024 + (((clut & 63) * 16 + tid) & 1023)];
            if (need_clut && !tid) { local.clut_id = ushort(clut); local.clut_valid = 1; local.clut_n = int(need); }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            for (uint i = tid; i < pixels; i += 256) {
                int x = box.x + int(i % width), y = box.y + int(i / width); int2 uv;
                if (!texture_position(o, grad, x, y, uv)) continue;
                uint4 address = texture_address(uv.x, uv.y, o.texpage, o.texwin); uint tag = address.y * 1024 + (address.x & ~3u);
                ushort sample = local.textag[address.z] == tag + 1 && i < atomic_load_explicit(first + address.z, memory_order_relaxed) ? local.texcache[address.z][address.x & 3] : vram[address.y * 1024 + address.x];
                int2 mapped = window_uv(uv.x, uv.y, o.texwin);
                if (address.w) sample = local.clutcache[address.w == 2 ? sample >> ((mapped.x & 3) * 4) & 15 : sample >> ((mapped.x & 1) * 8) & 255];
                samples[i] = uint(sample) | uint(mapped.x) << 16 | uint(mapped.y) << 24;
                int r = o.a.r, g = o.a.g, bl = o.a.b;
                if (o.flags & RENDER_GOURAUD) { r = interpolate(r, o.a, x, y, 1, grad, 0); g = interpolate(g, o.a, x, y, 1, grad, 1); bl = interpolate(bl, o.a, x, y, 1, grad, 2); }
                textured_plot(vram, x, y, 1, sample, r, g, bl, o);
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
            uint end = atomic_load_explicit(last + tid, memory_order_relaxed);
            if (end && atomic_load_explicit(first + tid, memory_order_relaxed) != 0xffffffffu) {
                uint at = end - 1; int2 uv; texture_position(o, grad, box.x + int(at % width), box.y + int(at / width), uv);
                uint4 address = texture_address(uv.x, uv.y, o.texpage, o.texwin); uint tag = address.y * 1024 + (address.x & ~3u);
                for (uint k = 0; k < 4; k++) local.texcache[tid][k] = vram[tag + k]; local.textag[tid] = tag + 1;
            }
            threadgroup_barrier(mem_flags::mem_threadgroup);
        } else {
            for (uint i = tid; i < pixels; i += 256) {
                int x = box.x + int(i % width), y = box.y + int(i / width); if (!inside(a, b, c, x, y, 1)) continue;
                ushort color = rgb15(0, 0, o.a.r, o.a.g, o.a.b, false);
                if (o.flags & RENDER_GOURAUD) color = rgb15(x, y, interpolate(o.a.r, o.a, x, y, 1, grad, 0), interpolate(o.a.g, o.a, x, y, 1, grad, 1), interpolate(o.a.b, o.a, x, y, 1, grad, 2), o.flags & RENDER_DITHER);
                plot(vram, x, y, 1, color, o.flags & RENDER_SEMI, o.mask, o.texpage);
            }
        }
        if (scale > 1) for (uint i = tid; i < pixels * scale * scale; i += 256) {
            int x = box.x * int(scale) + int(i % (width * scale)), y = box.y * int(scale) + int(i / (width * scale)), nx = x / int(scale), ny = y / int(scale);
            if (!inside(a, b, c, x, y, scale)) continue;
            int r = o.a.r, g = o.a.g, bl = o.a.b;
            if (o.flags & RENDER_GOURAUD) { r = interpolate(r, o.a, x, y, scale, grad, 0); g = interpolate(g, o.a, x, y, scale, grad, 1); bl = interpolate(bl, o.a, x, y, scale, grad, 2); }
            if (o.flags & RENDER_TEXTURE) {
                int u = interpolate(o.a.u, o.a, x, y, scale, grad, 3), v = interpolate(o.a.v, o.a, x, y, scale, grad, 4);
                if (o.flags & RENDER_UV_LIMIT) { u = clamp(u, int(o.p[0] & 0xffff), int(o.p[0] >> 16)); v = clamp(v, int(o.p[1] & 0xffff), int(o.p[1] >> 16)); }
                uint raw = samples[uint(ny - box.y) * width + uint(nx - box.x)]; int2 mapped = window_uv(u, v, o.texwin); uint key = uint(mapped.x) | uint(mapped.y) << 8;
                ushort sample = inside(a, b, c, nx, ny, 1) && raw >> 16 == key ? ushort(raw) : texel_extra(vram, &local, u, v, o);
                textured_plot(hires, x, y, scale, sample, r, g, bl, o);
            } else plot(hires, x, y, scale, rgb15(nx, ny, r, g, bl, o.flags & RENDER_DITHER), o.flags & RENDER_SEMI, o.mask, o.texpage);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
    }
    for (uint k = 0; k < 4; k++) cache.texcache[tid][k] = local.texcache[tid][k];
    cache.textag[tid] = local.textag[tid]; cache.clutcache[tid] = local.clutcache[tid];
    if (!tid) { cache.clut_id = local.clut_id; cache.clut_valid = local.clut_valid; cache.clut_n = local.clut_n; }
}
