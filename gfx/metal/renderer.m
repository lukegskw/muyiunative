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
    id<MTLComputePipelineState> native, scaled, upload, display, flat, sprites, texture_prepare, texture_track, texture_draw, texture_finish, triangle_prepare, triangle;
    id<MTLBuffer> vram, hires, cache, scratch, output;
    ps1_render_op *ops; size_t n, cap;
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
        r->sprites = pipeline(dev, lib, @"render_sprites");
        r->texture_prepare = pipeline(dev, lib, @"render_texture_prepare"); r->texture_track = pipeline(dev, lib, @"render_texture_track");
        r->texture_draw = pipeline(dev, lib, @"render_texture_draw"); r->texture_finish = pipeline(dev, lib, @"render_texture_finish");
        r->triangle_prepare = pipeline(dev, lib, @"render_triangle_prepare"); r->triangle = pipeline(dev, lib, @"render_triangle");
        r->vram = [dev newBufferWithBytes:m->gpu.vram length:sizeof m->gpu.vram options:MTLResourceStorageModeShared];
        r->cache = [dev newBufferWithBytes:m->gpu.texcache length:sizeof(ps1_render_cache) options:MTLResourceStorageModeShared];
        r->hires = [dev newBufferWithLength:sizeof m->gpu.vram * scale * scale options:MTLResourceStorageModeShared];
        r->scratch = [dev newBufferWithLength:4 << 20 options:MTLResourceStorageModeShared];
        if (!r->queue || !r->native || !r->scaled || !r->upload || !r->display || !r->flat || !r->sprites ||
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
    r->queue = nil; r->native = r->scaled = r->upload = r->display = nil;
    r->flat = nil;
    r->sprites = nil;
    r->texture_prepare = r->texture_track = r->texture_draw = r->texture_finish = nil;
    r->triangle_prepare = r->triangle = nil;
    r->vram = r->hires = r->cache = r->scratch = r->output = nil;
    free(r->ops); free(r);
}

static int finish(metal_renderer *r, id<MTLCommandBuffer> cb) {
    [cb commit]; [cb waitUntilCompleted];
    if (!cb || cb.status == MTLCommandBufferStatusError) { RED("metal: command execution failed: %s", cb.error ? cb.error.localizedDescription.UTF8String : "no command buffer"); r->failed = 1; }
    return r->failed ? -1 : 0;
}

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
// Feedback is excluded conservatively by the whole page and CLUT ranges; overlapping sources retain the ordered reference shader, never a guessed texture snapshot.
static int independent_texture(const ps1_render_op *o) {
    if ((o->type != RENDER_RECT && o->type != RENDER_TRIANGLE) || !(o->flags & RENDER_TEXTURE)) return 0;
    int x0, y0, x1, y1;
    if (o->type == RENDER_RECT) { x0 = o->a.x; y0 = o->a.y; x1 = x0 + (int)o->p[0] - 1; y1 = y0 + (int)o->p[1] - 1; }
    else { x0 = MIN(o->a.x, MIN(o->b.x, o->c.x)); y0 = MIN(o->a.y, MIN(o->b.y, o->c.y));
        x1 = MAX(o->a.x, MAX(o->b.x, o->c.x)); y1 = MAX(o->a.y, MAX(o->b.y, o->c.y)); }
    x0 = MAX(x0, o->ax1); y0 = MAX(y0, o->ay1); x1 = MIN(x1, o->ax2); y1 = MIN(y1, o->ay2);
    if (x0 < 0 || y0 < 0 || x1 > 1023 || y1 > 511 || x1 < x0 || y1 < y0) return 0;
    int depth = o->texpage >> 7 & 3, sy = (o->texpage >> 4 & 1) * 256, width = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    if (y1 >= sy && y0 <= sy + 255 && overlaps_x(x0, x1, (o->texpage & 15) * 64, width)) return 0;
    int cy = (o->clut & 0x7fff) >> 6;
    return depth >= 2 || cy < y0 || cy > y1 || !overlaps_x(x0, x1, (o->clut & 63) * 16, depth == 0 ? 16 : 256);
}

// The title's 280 small rectangles per busy frame made GPU command barriers dominate (46.65s versus 18.52s software). A <=16x16 texture footprint with no window
// cannot alias a cache slot to different tags. One workgroup can load its entries and shade it in parallel, then process the next sprite in original command order.
static int sprite(const ps1_render_op *o) {
    return o->type == RENDER_RECT && !o->texwin && o->p[0] <= 16 && o->p[1] <= 16 && independent_texture(o);
}

static int flush(metal_renderer *r) {
    if (r->failed) return -1;
    if (!r->n) return 0;
    @autoreleasepool {
        if (getenv("PS1_METAL_TRACE")) {
            size_t counts[9] = {0}; for (size_t i = 0; i < r->n; i++) if (r->ops[i].type < 9) counts[r->ops[i].type]++;
            LOG("metal: batch %llu, %zu operations (triangle %zu, line %zu, rectangle %zu, fill %zu, copy %zu, upload %zu)",
                (unsigned long long)r->batches, r->n, counts[1], counts[2], counts[3], counts[4], counts[5], counts[6]);
        }
        r->batches++;
        id<MTLBuffer> ops = [r->queue.device newBufferWithBytes:r->ops length:r->n * sizeof *r->ops options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [r->queue commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        if (!ops || !e) { RED("metal: cannot encode GPU operations"); r->failed = 1; return -1; }
        [e setBuffer:r->vram offset:0 atIndex:0]; [e setBuffer:r->hires offset:0 atIndex:1]; [e setBuffer:r->cache offset:0 atIndex:2];
        uint32_t scale = r->scale; [e setBytes:&scale length:sizeof scale atIndex:4]; [e setBuffer:r->scratch offset:0 atIndex:5];
        for (size_t i = 0; i < r->n; i++) {
            const ps1_render_op *o = r->ops + i; [e setBuffer:ops offset:i * sizeof *o atIndex:3];
            if (o->type == RENDER_UPLOAD) {
                uint32_t count = 1;
                while (i + count < r->n && r->ops[i + count].type == RENDER_UPLOAD && r->ops[i + count].mask == o->mask && r->ops[i + count].p[2] == o->p[2] + count) count++;
                [e setComputePipelineState:r->upload]; [e setBytes:&count length:sizeof count atIndex:6];
                [e dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(MIN(count, 128u), 1, 1)]; i += count - 1;
            } else if (sprite(o)) {
                uint32_t count = 1;
                while (i + count < r->n && sprite(r->ops + i + count)) count++;
                [e setComputePipelineState:r->sprites]; [e setBytes:&count length:sizeof count atIndex:6];
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
        [e endEncoding]; r->n = 0; return finish(r, cb);
    }
}

const uint16_t *metal_renderer_vram(metal_renderer *r) { return r && !flush(r) ? r->vram.contents : NULL; }

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

int metal_renderer_frame(metal_renderer *r, uint32_t *out, int cap_w, int cap_h, int *w, int *h, int verify) {
    if (!r || flush(r) || (verify && metal_renderer_check(r))) return -1;
    @autoreleasepool {
        const ps1_gpu *g = &r->m->gpu; static const int widths[] = {256, 320, 512, 640};
        int lines = MIN(MAX(0, (int)g->vy2 - g->vy1), g->dmode & 8 ? 288 : 240);
        int width = MIN(g->dmode & 64 ? 368 : widths[g->dmode & 3], cap_w / r->scale), height = MIN((g->dmode & 0x24) == 0x24 ? lines * 2 : lines, cap_h / r->scale);
        *w = width * r->scale; *h = height * r->scale; size_t bytes = (size_t)*w * *h * sizeof *out;
        if (!bytes) return 0;
        if (r->output.length < bytes) r->output = [r->queue.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [r->queue commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        if (!r->output || !e) { RED("metal: cannot encode display scanout"); r->failed = 1; return -1; }
        uint32_t p[] = {g->disp_x, g->disp_y, g->dmode, (uint32_t)g->display_off, (uint32_t)*w, (uint32_t)*h, (uint32_t)r->scale};
        [e setComputePipelineState:r->display]; [e setBuffer:r->vram offset:0 atIndex:0]; [e setBuffer:r->hires offset:0 atIndex:1];
        [e setBytes:p length:sizeof p atIndex:2]; [e setBuffer:r->output offset:0 atIndex:3];
        [e dispatchThreads:MTLSizeMake(*w, *h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; [e endEncoding];
        if (finish(r, cb)) return -1;
        memcpy(out, r->output.contents, bytes); return 0;
    }
}
