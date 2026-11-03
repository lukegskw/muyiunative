// Independent Metal command rendering (plan.md, phase 6; issue #11). Software VRAM remains the console's CPU-visible reference; this renderer owns its own VRAM and
// caches and draws the ordered GPU operations, never the software frame. Retaining the reference has a CPU cost, measured in docs/development.md.
// Apple Metal resource synchronization: tracked shared buffers plus explicit encoder barriers preserve command order; wait for completion before CPU readback.
// The metallib is compiled by xcrun at build time and loaded with newLibraryWithURL:error:, never newLibraryWithSource:.
#import <Metal/Metal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "renderer.h"

struct metal_renderer {
    ps1 *m; int scale, failed;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> native, scaled, upload, display, flat, copy, sprites, triangles, texture_prepare, texture_track, texture_draw, texture_finish, triangle_prepare, triangle;
    id<MTLBuffer> vram, hires, cache, scratch, output;
    ps1_render_op *ops; size_t n, cap;
    id<MTLCommandBuffer> pending[2]; id<MTLBuffer> frames[2]; int pending_w[2], pending_h[2], pending_read, pending_count;
    uint64_t batches;
};
_Static_assert(sizeof(ps1_render_op) == 140, "shared shader operation ABI");
_Static_assert(offsetof(ps1_gpu, ax1) - offsetof(ps1_gpu, texcache) == sizeof(ps1_render_cache), "shared shader cache ABI");

static void emit(void *ctx, const ps1_render_op *op) {
    metal_renderer *r = ctx;
    if (r->failed) return;
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 4096;
        ps1_render_op *p = realloc(r->ops, cap * sizeof *p);
        if (!p) { RED("metal: no memory for GPU operations"); r->failed = 1; return; }
        r->ops = p; r->cap = cap;
    }
    r->ops[r->n++] = *op;
}

static id<MTLComputePipelineState> pipeline(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *name) {
    NSError *error = nil; id<MTLFunction> fn = [lib newFunctionWithName:name];
    id<MTLComputePipelineState> p = fn ? [dev newComputePipelineStateWithFunction:fn error:&error] : nil;
    if (!p) RED("metal: cannot load %s: %s", name.UTF8String, error ? error.localizedDescription.UTF8String : "shader missing from metallib");
    return p;
}

metal_renderer *metal_renderer_new(ps1 *m, const char *path, int scale) {
    @autoreleasepool {
        if (!m || m->on_render || (scale != 1 && scale != 2 && scale != 4)) { RED("metal: invalid machine, renderer already attached, or scale (use 1, 2, 4)"); return NULL; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *error = nil;
        id<MTLLibrary> lib = dev && path ? [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error] : nil;
        if (!lib) { RED("metal: cannot open offline shader library: %s", error ? error.localizedDescription.UTF8String : "no Metal device or metallib"); return NULL; }
        metal_renderer *r = calloc(1, sizeof *r); if (!r) return NULL;
        r->m = m; r->scale = scale; r->queue = [dev newCommandQueue];
        r->native = pipeline(dev, lib, @"render_native"); r->scaled = pipeline(dev, lib, @"render_scaled");
        r->upload = pipeline(dev, lib, @"render_upload"); r->display = pipeline(dev, lib, @"render_display");
        r->flat = pipeline(dev, lib, @"render_flat");
        r->copy = pipeline(dev, lib, @"render_copy");
        r->sprites = pipeline(dev, lib, @"render_sprites");
        r->triangles = pipeline(dev, lib, @"render_triangles");
        r->texture_prepare = pipeline(dev, lib, @"render_texture_prepare"); r->texture_track = pipeline(dev, lib, @"render_texture_track");
        r->texture_draw = pipeline(dev, lib, @"render_texture_draw"); r->texture_finish = pipeline(dev, lib, @"render_texture_finish");
        r->triangle_prepare = pipeline(dev, lib, @"render_triangle_prepare"); r->triangle = pipeline(dev, lib, @"render_triangle");
        r->vram = [dev newBufferWithBytes:m->gpu.vram length:sizeof m->gpu.vram options:MTLResourceStorageModeShared];
        r->cache = [dev newBufferWithBytes:m->gpu.texcache length:sizeof(ps1_render_cache) options:MTLResourceStorageModeShared];
        r->hires = [dev newBufferWithLength:sizeof m->gpu.vram * scale * scale options:MTLResourceStorageModeShared];
        r->scratch = [dev newBufferWithLength:4 << 20 options:MTLResourceStorageModeShared];
        if (!r->queue || !r->native || !r->scaled || !r->upload || !r->display || !r->flat || !r->copy || !r->sprites || !r->triangles ||
            !r->texture_prepare || !r->texture_track || !r->texture_draw || !r->texture_finish || !r->triangle_prepare || !r->triangle ||
            !r->vram || !r->cache || !r->hires || !r->scratch) {
            RED("metal: cannot allocate renderer resources"); metal_renderer_free(r); return NULL;
        }
        uint16_t *h = r->hires.contents;
        for (int y = 0; y < 512 * scale; y++) for (int x = 0; x < 1024 * scale; x++) h[y * 1024 * scale + x] = m->gpu.vram[y / scale * 1024 + x / scale];
        m->render_ctx = r; m->on_render = emit;
        LOG("metal: %s, %dx internal resolution, offline shaders", dev.name.UTF8String, scale);
        return r;
    }
}

void metal_renderer_free(metal_renderer *r) {
    if (!r) return;
    if (r->m && r->m->render_ctx == r) { r->m->on_render = NULL; r->m->render_ctx = NULL; }
    for (int i = 0; i < 2; i++) { [r->pending[i] waitUntilCompleted]; r->pending[i] = nil; r->frames[i] = nil; }
    r->queue = nil; r->native = r->scaled = r->upload = r->display = nil;
    r->flat = r->copy = nil;
    r->sprites = r->triangles = nil;
    r->texture_prepare = r->texture_track = r->texture_draw = r->texture_finish = nil;
    r->triangle_prepare = r->triangle = nil;
    r->vram = r->hires = r->cache = r->scratch = r->output = nil;
    free(r->ops); free(r);
}

static int completed(metal_renderer *r, id<MTLCommandBuffer> cb) {
    [cb waitUntilCompleted];
    if (!cb || cb.status == MTLCommandBufferStatusError) { RED("metal: command execution failed: %s", cb.error ? cb.error.localizedDescription.UTF8String : "no command buffer"); r->failed = 1; }
    return r->failed ? -1 : 0;
}

static int finish(metal_renderer *r, id<MTLCommandBuffer> cb) { [cb commit]; return completed(r, cb); }

static MTLSize bounds(const ps1_render_op *o, int scale) {
    int x0, x1, y0, y1;
    if (o->type == RENDER_FILL) return MTLSizeMake((((o->p[2] & 1023) + 15u) & ~15u) * scale, (o->p[2] >> 16 & 511) * scale, 1);
    if (o->type == RENDER_RECT) { x0 = o->a.x; y0 = o->a.y; x1 = x0 + (int)o->p[0] - 1; y1 = y0 + (int)o->p[1] - 1; }
    else {
        x0 = MIN(o->a.x, o->b.x); x1 = MAX(o->a.x, o->b.x); y0 = MIN(o->a.y, o->b.y); y1 = MAX(o->a.y, o->b.y);
        if (o->type == RENDER_TRIANGLE) { x0 = MIN(x0, o->c.x); x1 = MAX(x1, o->c.x); y0 = MIN(y0, o->c.y); y1 = MAX(y1, o->c.y); }
    }
    x0 = MAX(x0, o->ax1); x1 = MIN(x1, o->ax2); y0 = MAX(y0, o->ay1); y1 = MIN(y1, o->ay2);
    return MTLSizeMake((NSUInteger)MAX(0, x1 - x0 + 1) * scale, (NSUInteger)MAX(0, y1 - y0 + 1) * scale, 1);
}

static int overlaps_x(int x0, int x1, int source, int width) {
    int end = source + width - 1;
    return (x1 >= source && x0 <= MIN(end, 1023)) || (end >= 1024 && x0 <= end - 1024);
}

static int copy_overlap(unsigned a, unsigned b, unsigned length, unsigned size) {
    return ((a - b) & (size - 1)) < length || ((b - a) & (size - 1)) < length;
}
// Crash's boot copies 512x240 pixels between disjoint buffers; the single-lane 4x copy took ~294 ms. Parallel writes are safe only for disjoint rectangles or an
// identity copy, including VRAM wrapping. Other overlaps keep row snapshots and vertical propagation (psx-spx, GPU Memory Transfer Commands / Wrapping).
static int independent_copy(const ps1_render_op *o) {
    unsigned sx = o->p[0] & 1023, sy = o->p[0] >> 16 & 511, dx = o->p[1] & 1023, dy = o->p[1] >> 16 & 511;
    unsigned w = ((o->p[2] - 1) & 1023) + 1, h = (((o->p[2] >> 16) - 1) & 511) + 1;
    return (sx == dx && sy == dy) || !copy_overlap(sx, dx, w, 1024) || !copy_overlap(sy, dy, h, 512);
}
// FF IX's first battle sent 98 apparent feedback primitives in a busy frame: checking an entire texture page classified unrelated framebuffer pixels as sources.
// Bound the requested UVs, then apply the window/wrapping, including every word of each fetched cache line and the full CLUT (psx-spx, GPU Texture Caching).
// FF IX's static crate alternated fine geometry between its x=0/320 buffers: padding U=127 to 128 invented a wrapped read at x=0. For a legal 1023x511 polygon,
// truncating both 12-bit gradients introduces <1534/4096 texel of error. Nearest rounding therefore stays within the integer vertex UV extrema (psx-spx, Vertex
// size restriction; the reference's measured interpolation). Use those bounds without padding; texture-window mapping and full cache-line/CLUT ranges still apply.
static int independent_texture(const ps1_render_op *o) {
    if ((o->type != RENDER_RECT && o->type != RENDER_TRIANGLE) || !(o->flags & RENDER_TEXTURE)) return 0;
    if (o->flags & RENDER_CLASSIFIED) return !(o->flags & RENDER_FEEDBACK);
    int x0, y0, x1, y1;
    if (o->type == RENDER_RECT) { x0 = o->a.x; y0 = o->a.y; x1 = x0 + (int)o->p[0] - 1; y1 = y0 + (int)o->p[1] - 1; }
    else { x0 = MIN(o->a.x, MIN(o->b.x, o->c.x)); y0 = MIN(o->a.y, MIN(o->b.y, o->c.y));
        x1 = MAX(o->a.x, MAX(o->b.x, o->c.x)); y1 = MAX(o->a.y, MAX(o->b.y, o->c.y)); }
    x0 = MAX(x0, o->ax1); y0 = MAX(y0, o->ay1); x1 = MIN(x1, o->ax2); y1 = MIN(y1, o->ay2);
    if (x0 < 0 || y0 < 0 || x1 > 1023 || y1 > 511 || x1 < x0 || y1 < y0) return 0;
    int u0, u1, v0, v1;
    if (o->type == RENDER_RECT) {
        int u = o->a.u + (o->texpage & 0x1000 ? 1 : 0), du = (int)o->p[0] - 1, dv = (int)o->p[1] - 1;
        u0 = o->texpage & 0x1000 ? u - du : u; u1 = o->texpage & 0x1000 ? u : u + du;
        v0 = o->texpage & 0x2000 ? o->a.v - dv : o->a.v; v1 = o->texpage & 0x2000 ? o->a.v : o->a.v + dv;
    } else {
        u0 = MIN(o->a.u, MIN(o->b.u, o->c.u)); u1 = MAX(o->a.u, MAX(o->b.u, o->c.u));
        v0 = MIN(o->a.v, MIN(o->b.v, o->c.v)); v1 = MAX(o->a.v, MAX(o->b.v, o->c.v));
    }
    int depth = o->texpage >> 7 & 3, shift = depth == 0 ? 2 : depth == 1 ? 1 : 0;
    int mx = (o->texwin & 31) * 8, my = (o->texwin >> 5 & 31) * 8, source_x = 0, source_y = 0;
    for (int u = u0, end = MIN(u1, u0 + 255); u <= end; u++) {
        int uv = ((u & 255) & ~mx) | (((o->texwin >> 10 & 31) * 8) & mx);
        int x = (((o->texpage & 15) * 64 + (uv >> shift)) & 1023) & ~3;
        if (x1 >= x && x0 <= x + 3) { source_x = 1; break; }
    }
    for (int v = v0, end = MIN(v1, v0 + 255); v <= end; v++) {
        int uv = ((v & 255) & ~my) | (((o->texwin >> 15 & 31) * 8) & my);
        int y = (o->texpage >> 4 & 1) * 256 + uv;
        if (y >= y0 && y <= y1) { source_y = 1; break; }
    }
    if (source_x && source_y) return 0;
    int cy = (o->clut & 0x7fff) >> 6;
    return depth >= 2 || cy < y0 || cy > y1 || !overlaps_x(x0, x1, (o->clut & 63) * 16, depth == 0 ? 16 : 256);
}

// The title's 280 small rectangles per busy frame made GPU command barriers dominate (46.65s versus 18.52s software). A <=16x16 texture footprint with no window
// cannot alias a cache slot to different tags. One workgroup can load its entries and shade it in parallel, then process the next sprite in original command order.
static int sprite(const ps1_render_op *o) {
    return o->type == RENDER_RECT && !o->texwin && o->p[0] <= 16 && o->p[1] <= 16 && independent_texture(o);
}

static int small_triangle(const ps1_render_op *o) {
    if (o->type != RENDER_TRIANGLE || ((o->flags & RENDER_TEXTURE) && !independent_texture(o))) return 0;
    MTLSize size = bounds(o, 1);
    return size.width && size.height && size.width * size.height <= 256;
}

// Crash's busy frame took ~29.7 ms of GPU work with ~0.12 ms of CPU encoding. Preparing its exact 12-bit gradients on the CPU reduced that GPU work to ~23.7 ms,
// keeping signed 64-bit division out of the shaders. Coefficients retain the reference's truncation; the GPU still rasterizes every native/fine pixel.
static void triangle_gradients(const ps1_render_op *o, int64_t *grad) {
    ps1_vtx a = o->a, b = o->b, c = o->c;
    int64_t bx = b.x - a.x, by = b.y - a.y, cx = c.x - a.x, cy = c.y - a.y, area = bx * cy - by * cx;
    if (!area) { memset(grad, 0, 10 * sizeof *grad); return; }
    int fa[] = {a.r, a.g, a.b, a.u, a.v}, fb[] = {b.r, b.g, b.b, b.u, b.v}, fc[] = {c.r, c.g, c.b, c.u, c.v};
    for (int i = 0; i < 5; i++) {
        int64_t nb = fb[i] - fa[i], nc = fc[i] - fa[i];
        grad[i] = (nb * cy - nc * by) * 4096 / area; grad[i + 5] = (nc * bx - nb * cx) * 4096 / area;
    }
}

static int encode(metal_renderer *r, id<MTLCommandBuffer> cb) {
    if (r->failed) return -1;
    if (!r->n) return 0;
    @autoreleasepool {
        if (getenv("PS1_METAL_TRACE")) {
            size_t counts[9] = {0}; for (size_t i = 0; i < r->n; i++) if (r->ops[i].type < 9) counts[r->ops[i].type]++;
            LOG("metal: batch %llu, %zu operations (triangle %zu, line %zu, rectangle %zu, fill %zu, copy %zu, upload %zu)",
                (unsigned long long)r->batches, r->n, counts[1], counts[2], counts[3], counts[4], counts[5], counts[6]);
        }
        r->batches++;
        size_t triangles = 0;
        for (size_t i = 0; i < r->n; i++) {
            ps1_render_op *o = r->ops + i;
            if (o->flags & RENDER_TEXTURE) { if (!independent_texture(o)) o->flags |= RENDER_FEEDBACK; o->flags |= RENDER_CLASSIFIED; }
            if (o->type == RENDER_TRIANGLE) o->p[3] = (uint32_t)triangles++;
        }
        id<MTLBuffer> gradients = [r->queue.device newBufferWithLength:MAX(triangles, 1) * 10 * sizeof(int64_t) options:MTLResourceStorageModeShared];
        if (!gradients) { RED("metal: cannot allocate triangle gradients"); r->failed = 1; return -1; }
        int64_t *coefficients = gradients.contents;
        for (size_t i = 0; i < r->n; i++) if (r->ops[i].type == RENDER_TRIANGLE) triangle_gradients(r->ops + i, coefficients + r->ops[i].p[3] * 10);
        id<MTLBuffer> ops = [r->queue.device newBufferWithBytes:r->ops length:r->n * sizeof *r->ops options:MTLResourceStorageModeShared];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        if (!ops || !e) { RED("metal: cannot encode GPU operations"); r->failed = 1; return -1; }
        [e setBuffer:r->vram offset:0 atIndex:0]; [e setBuffer:r->hires offset:0 atIndex:1]; [e setBuffer:r->cache offset:0 atIndex:2];
        uint32_t scale = r->scale; [e setBytes:&scale length:sizeof scale atIndex:4]; [e setBuffer:r->scratch offset:0 atIndex:5]; [e setBuffer:gradients offset:0 atIndex:7];
        for (size_t i = 0; i < r->n; i++) {
            const ps1_render_op *o = r->ops + i; [e setBuffer:ops offset:i * sizeof *o atIndex:3];
            if (o->type == RENDER_UPLOAD) {
                uint32_t count = 1;
                while (i + count < r->n && r->ops[i + count].type == RENDER_UPLOAD && r->ops[i + count].mask == o->mask && r->ops[i + count].p[2] == o->p[2] + count) count++;
                [e setComputePipelineState:r->upload]; [e setBytes:&count length:sizeof count atIndex:6];
                [e dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(MIN(count, 128u), 1, 1)]; i += count - 1;
            } else if (o->type == RENDER_COPY && independent_copy(o)) {
                MTLSize size = MTLSizeMake(((o->p[2] - 1) & 1023) + 1, (((o->p[2] >> 16) - 1) & 511) + 1, 1);
                [e setComputePipelineState:r->copy]; [e dispatchThreads:size threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            } else if (sprite(o)) {
                uint32_t count = 1;
                while (i + count < r->n && sprite(r->ops + i + count)) count++;
                [e setComputePipelineState:r->sprites]; [e setBytes:&count length:sizeof count atIndex:6];
                [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; i += count - 1;
            } else if (small_triangle(o)) {
                uint32_t count = 1;
                while (i + count < r->n && small_triangle(r->ops + i + count)) count++;
                [e setComputePipelineState:r->triangles]; [e setBytes:&count length:sizeof count atIndex:6];
                [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; i += count - 1;
            } else if (independent_texture(o)) {
                MTLSize size = bounds(o, 1), slots = MTLSizeMake(256, 1, 1), group = MTLSizeMake(128, 1, 1);
                [e setComputePipelineState:r->texture_prepare]; [e dispatchThreads:slots threadsPerThreadgroup:group];
                [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
                [e setComputePipelineState:r->texture_track]; [e dispatchThreads:size threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
                [e setComputePipelineState:r->texture_draw]; [e dispatchThreads:size threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
                [e setComputePipelineState:r->texture_finish]; [e dispatchThreads:slots threadsPerThreadgroup:group];
                if (o->type == RENDER_TRIANGLE && r->scale > 1) {
                    [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
                    [e setComputePipelineState:r->scaled]; [e dispatchThreads:bounds(o, r->scale) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                }
            } else if (o->type == RENDER_FILL || (o->type == RENDER_RECT && !(o->flags & RENDER_TEXTURE))) {
                MTLSize size = bounds(o, 1);
                if (size.width && size.height) { [e setComputePipelineState:r->flat]; [e dispatchThreads:size threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; }
            } else if (o->type == RENDER_TRIANGLE && !(o->flags & RENDER_TEXTURE)) {
                MTLSize size = bounds(o, 1);
                if (size.width && size.height) {
                    if (o->flags & RENDER_GOURAUD) {
                        [e setComputePipelineState:r->triangle_prepare];
                        [e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                        [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
                    }
                    [e setComputePipelineState:r->triangle]; [e dispatchThreads:size threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                    if (r->scale > 1) {
                        [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
                        [e setComputePipelineState:r->scaled]; [e dispatchThreads:bounds(o, r->scale) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                    }
                }
            } else {
                [e setComputePipelineState:r->native]; [e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                if (r->scale > 1 && o->type >= RENDER_TRIANGLE && o->type <= RENDER_FILL) {
                    [e memoryBarrierWithScope:MTLBarrierScopeBuffers]; MTLSize size = bounds(o, r->scale);
                    if (size.width && size.height) { [e setComputePipelineState:r->scaled]; [e dispatchThreads:size threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; }
                }
            }
            [e memoryBarrierWithScope:MTLBarrierScopeBuffers];
        }
        [e endEncoding]; r->n = 0; return 0;
    }
}

static int flush(metal_renderer *r) {
    if (r->failed) return -1;
    if (!r->n && !r->pending_count) return 0;
    @autoreleasepool { id<MTLCommandBuffer> cb = [r->queue commandBuffer]; return encode(r, cb) ? -1 : finish(r, cb); }
}

const uint16_t *metal_renderer_vram(metal_renderer *r) { return r && !flush(r) ? r->vram.contents : NULL; }

int metal_renderer_save_state(metal_renderer *r, uint16_t *fine, size_t words) {
    if (!r || !fine || words != (size_t)VRAM_W * VRAM_H * r->scale * r->scale || flush(r)) return -1;
    memcpy(fine, r->hires.contents, words * sizeof *fine); return 0;
}

int metal_renderer_load_state(metal_renderer *r, const uint16_t *fine, int source_scale) {
    // Use a fresh renderer after restoring console state, so no queued image can overwrite the restored fine samples. A changed graphics choice resamples the
    // saved grid; it does not change the console or the player's preference. The next draws use the selected resolution normally.
    if (!r || !fine || (source_scale != 2 && source_scale != 4) || r->n || r->pending_count) return -1;
    uint16_t *out = r->hires.contents;
    for (int y = 0; y < VRAM_H * r->scale; y++) for (int x = 0; x < VRAM_W * r->scale; x++)
        out[y * VRAM_W * r->scale + x] = fine[(y * source_scale / r->scale) * VRAM_W * source_scale + x * source_scale / r->scale];
    return 0;
}

int metal_renderer_check(metal_renderer *r) {
    if (!r || flush(r)) return -1;
    const uint16_t *v = r->vram.contents;
    if (memcmp(v, r->m->gpu.vram, sizeof r->m->gpu.vram)) {
        size_t p = 0; while (v[p] == r->m->gpu.vram[p]) p++;
        RED("metal: software mismatch at VRAM (%zu,%zu): software %04x, Metal %04x", p % 1024, p / 1024, r->m->gpu.vram[p], v[p]); return -1;
    }
    if (memcmp(r->cache.contents, r->m->gpu.texcache, sizeof(ps1_render_cache))) { RED("metal: software mismatch in texture/CLUT cache"); return -1; }
    return 0;
}

static int scanout(metal_renderer *r, id<MTLCommandBuffer> cb, int cap_w, int cap_h, int *w, int *h) {
    @autoreleasepool {
        const ps1_gpu *g = &r->m->gpu; static const int widths[] = {256, 320, 512, 640};
        int lines = MIN(MAX(0, (int)g->vy2 - g->vy1), g->dmode & 8 ? 288 : 240);
        int width = MIN(g->dmode & 64 ? 368 : widths[g->dmode & 3], cap_w / r->scale), height = MIN((g->dmode & 0x24) == 0x24 ? lines * 2 : lines, cap_h / r->scale);
        *w = width * r->scale; *h = height * r->scale; size_t bytes = (size_t)*w * *h * sizeof(uint32_t);
        if (!bytes) return 0;
        if (r->output.length < bytes) r->output = [r->queue.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        if (!r->output || !e) { RED("metal: cannot encode display scanout"); r->failed = 1; return -1; }
        uint32_t p[] = {g->disp_x, g->disp_y, g->dmode, (uint32_t)g->display_off, (uint32_t)*w, (uint32_t)*h, (uint32_t)r->scale};
        [e setComputePipelineState:r->display]; [e setBuffer:r->vram offset:0 atIndex:0]; [e setBuffer:r->hires offset:0 atIndex:1];
        [e setBytes:p length:sizeof p atIndex:2]; [e setBuffer:r->output offset:0 atIndex:3];
        [e dispatchThreads:MTLSizeMake(*w, *h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; [e endEncoding];
        return 0;
    }
}

int metal_renderer_frame(metal_renderer *r, uint32_t *out, int cap_w, int cap_h, int *w, int *h, int verify) {
    if (!r || flush(r) || (verify && metal_renderer_check(r))) return -1;
    for (int i = 0; i < 2; i++) r->pending[i] = nil;
    r->pending_count = r->pending_read = 0;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [r->queue commandBuffer];
        if (scanout(r, cb, cap_w, cap_h, w, h) || finish(r, cb)) return -1;
        if (*w && *h) memcpy(out, r->output.contents, (size_t)*w * *h * sizeof *out);
        return 0;
    }
}

// Crash alternates expensive draws with idle fields: synchronously waiting for each draw delays the following field's SPU samples despite adequate average
// throughput. Keep two submitted frames while the CPU produces the next: the measured one-frame pipeline still starved three callback periods in 600 Crash fields.
// Each scanout snapshots its own registers and finishes before its output is reused;
// no machine state crosses threads, no frame is skipped, and audio capacity/guest timing stay unchanged. Reference checks use the synchronous API above.
int metal_renderer_frame_async(metal_renderer *r, uint32_t *out, int cap_w, int cap_h, int *w, int *h) {
    if (!r || r->failed) return -1;
    @autoreleasepool {
        *w = *h = 0;
        if (r->pending_count == 2) {
            int slot = r->pending_read;
            if (completed(r, r->pending[slot])) return -1;
            if (r->pending_w[slot] > cap_w || r->pending_h[slot] > cap_h) { RED("metal: pending frame exceeds output capacity"); return -1; }
            *w = r->pending_w[slot]; *h = r->pending_h[slot];
            if (*w && *h) memcpy(out, r->frames[slot].contents, (size_t)*w * *h * sizeof *out);
            r->pending[slot] = nil; r->pending_read = (slot + 1) % 2; r->pending_count--;
        }
        int slot = (r->pending_read + r->pending_count) % 2;
        r->output = r->frames[slot];
        id<MTLCommandBuffer> cb = [r->queue commandBuffer];
        if (encode(r, cb) || scanout(r, cb, cap_w, cap_h, &r->pending_w[slot], &r->pending_h[slot])) return -1;
        r->frames[slot] = r->output;
        [cb commit]; r->pending[slot] = cb; r->pending_count++;
        return 0;
    }
}
