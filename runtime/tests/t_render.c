// Phase 6's ordered rendering boundary: completed GP0 primitives and transfers reach the owning machine's host before the unchanged software GPU draws them.
// Synthetic packets check snapshots, partial packets, polylines, odd uploads and cache/reset distinctions (psx-spx, Graphics Processing Unit). No device or game data.
#include "ps1.h"
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(ps1_render_op) == 140, "host/shader operation ABI");
_Static_assert(sizeof(ps1_render_cache) == offsetof(ps1_gpu, ax1) - offsetof(ps1_gpu, texcache), "host/shader cache ABI");
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
#define V(x,y) ((uint32_t)(uint16_t)(y) << 16 | (uint16_t)(x))
#define UV(u,v,clut) ((uint32_t)(clut) << 16 | (uint32_t)(v) << 8 | (u))
typedef struct recording { ps1 *m; ps1_render_op op[128]; uint16_t before[128]; unsigned n; } recording;
static void record(void *ctx, const ps1_render_op *op) {
    recording *r = ctx;
    if (r->n < sizeof r->op / sizeof *r->op) { r->op[r->n] = *op; r->before[r->n] = r->m->gpu.vram[0]; }
    r->n++;
}
static void packet(ps1 *a, ps1 *b, const uint32_t *words, size_t n) { for (size_t i = 0; i < n; i++) { gpu_gp0(a, words[i]); gpu_gp0(b, words[i]); } }
#define GP0(...) do { const uint32_t words[] = {__VA_ARGS__}; packet(a, b, words, sizeof words / sizeof *words); } while (0)
static void gp1(ps1 *a, ps1 *b, uint32_t w) { gpu_gp1(a, w); gpu_gp1(b, w); }

int main(void) {
    ps1 *a = ps1_new(), *b = ps1_new(), *other = ps1_new();
    if (!a || !b || !other) { fputs("FAIL: no machines for rendering test\n", stderr); ps1_free(a); ps1_free(b); ps1_free(other); return 1; }
    recording r = {.m = a}, separate = {.m = other}; a->on_render = record; a->render_ctx = &r;
    other->on_render = record; other->render_ctx = &separate;
    GP0(0xe3000000 | 1 | 2 << 10, 0xe4000000 | 40 | 40 << 10, 0xe5000000 | 3 | 0x7fe << 11, 0xe1000200);
    CHECK(!r.n, "drawing-register changes are captured by later operations, not separate primitives");
    GP0(0x32010203, V(10,10), 0x00040506, V(14,10), 0x00070809);
    CHECK(!r.n && a->gpu.n == 5, "an incomplete shaded triangle emits nothing");
    GP0(V(10,14));
    CHECK(r.n == 1 && r.op[0].type == RENDER_TRIANGLE, "the completed triangle emits once");
    CHECK(r.op[0].flags == (RENDER_GOURAUD | RENDER_SEMI | RENDER_DITHER), "triangle flags preserve shading, transparency and dithering");
    CHECK(r.op[0].a.x == 13 && r.op[0].a.y == 8 && r.op[0].a.r == 3 && r.op[0].a.g == 2 && r.op[0].a.b == 1, "vertices include the signed drawing offset and channels");
    CHECK(r.op[0].ax1 == 1 && r.op[0].ay1 == 2 && r.op[0].ax2 == 40 && r.op[0].ay2 == 40 && r.op[0].texpage == 0x200, "operations own snapshots of drawing state");
    CHECK(!r.op[0].clut && !r.op[0].p[0] && !r.op[0].p[3], "unused descriptor fields are zero");

    GP0(0xe1003200, 0xe2001234, 0xe6000001);
    unsigned n = r.n, clut = 3 | 300 << 6;
    GP0(0x2f808080, V(10,10), UV(1,2,clut), V(14,10), UV(5,2,0x100), V(10,14), UV(1,6,0), V(14,14), UV(5,6,0));
    CHECK(r.n == n + 2 && r.op[n].type == RENDER_TRIANGLE && r.op[n+1].type == RENDER_TRIANGLE, "a textured quad emits its two triangles in order");
    CHECK(r.op[n].a.x == 13 && r.op[n+1].a.x == 17, "the second quad triangle starts at vertex two");
    CHECK(r.op[n].flags == (RENDER_TEXTURE | RENDER_SEMI | RENDER_RAW) && r.op[n].clut == clut && r.op[n].a.u == 1 && r.op[n].a.v == 2, "raw textured triangles preserve UVs and CLUT without dithering");
    CHECK(r.op[n].texpage == 0x3300 && r.op[n].texwin == 0x1234 && r.op[n].mask == 1 && r.op[0].texpage == 0x200, "polygon page updates are captured without mutating earlier descriptors");
    n = r.n;
    GP0(0x200000ff, V(1,1), V(2,2), V(3,3), 0x200000ff, V(-1024,0), V(1023,0), V(0,1), 0x400000ff, V(-1024,0), V(1023,0));
    CHECK(r.n == n, "degenerate triangles and oversized triangles or lines emit no draws");
    GP0(0x64808080, V(12,12), UV(7,9,clut), V(4,3));
    CHECK(r.n == n + 1 && r.op[n].type == RENDER_RECT && r.op[n].p[0] == 4 && r.op[n].p[1] == 3, "rectangles carry decoded dimensions");
    CHECK(r.op[n].flags == RENDER_TEXTURE && r.op[n].a.u == 7 && r.op[n].a.v == 9 && r.op[n].clut == clut, "rectangle texture origin is distinct from its offset destination");
    n = r.n; GP0(0x600000ff, V(12,12), V(0,1)); CHECK(r.n == n, "zero-width rectangles emit no draw");

    GP0(0x480000ff, V(2,2), V(5,2), V(5,5), 0x50005000);
    CHECK(r.n == n + 2 && r.op[n].type == RENDER_LINE && r.op[n+1].type == RENDER_LINE && !a->gpu.poly, "a polyline emits segments, not its terminator");
    CHECK(r.op[n].b.x == r.op[n+1].a.x && r.op[n].b.y == r.op[n+1].a.y && r.op[n].flags == RENDER_DITHER, "adjacent segments share their decoded endpoint");
    n = r.n; GP0(0x5a0000ff, V(4,4), 0x0000ff00, V(8,4), 0x00ff0000, V(8,8), 0x50005000);
    CHECK(r.n == n + 2 && r.op[n].flags == (RENDER_GOURAUD | RENDER_SEMI | RENDER_DITHER) && r.op[n].a.r == 255 && r.op[n].b.g == 255 && r.op[n+1].b.b == 255,
          "shaded polylines retain each segment's endpoint colors and flags");

    n = r.n; GP0(0xa0000000, V(1023,511), V(3,1)); CHECK(r.n == n, "upload headers do not emit unreceived pixels");
    GP0(0x22221111, 0xdead3333);
    CHECK(r.n == n + 3 && r.op[n].type == RENDER_UPLOAD && r.op[n+2].p[1] == 0x3333 && !a->gpu.xfer, "odd uploads emit three pixels and discard their padding halfword");
    CHECK(r.op[n].p[0] == VRAM_W * VRAM_H - 1 && r.op[n+1].p[0] == 511 * VRAM_W && r.op[n+2].p[0] == 511 * VRAM_W + 1, "upload indices wrap horizontally within their VRAM row");
    CHECK(r.op[n].p[2] == 0 && r.op[n+1].p[2] == 1 && r.op[n+2].p[2] == 2 && r.op[n].mask == 1, "uploads preserve transfer order and the mask policy");
    n = r.n; GP0(0xe6000002, 0xa0000000, V(1023,511), V(1,1), 7);
    CHECK(r.n == n + 1 && r.op[n].p[1] == 7 && r.op[n].mask == 2 && a->gpu.vram[VRAM_W * VRAM_H - 1] == 0x9111, "masked uploads remain operations even when the reference preserves its destination");
    n = r.n; GP0(0xc0000000, V(1023,511), V(3,1));
    CHECK(gpu_read(a) == gpu_read(b) && gpu_read(a) == gpu_read(b) && r.n == n, "VRAM readback changes no rendering state");
    GP0(0x02f0a050, V(15,511), V(17,2));
    CHECK(r.n == n + 1 && r.op[n].type == RENDER_FILL && r.op[n].p[0] == 0xf0a050 && r.op[n].p[1] == V(15,511) && r.op[n].p[2] == V(17,2), "fills preserve their raw alignment and wrapping parameters");
    CHECK(!r.before[n] && a->gpu.vram[0] != 0, "the consumer sees the operation before the reference writes its pixels");
    n = r.n; GP0(0x80000000, V(0,0), V(1,0), V(3,1));
    CHECK(r.n == n + 1 && r.op[n].type == RENDER_COPY && r.op[n].p[0] == V(0,0) && r.op[n].p[1] == V(1,0) && r.op[n].p[2] == V(3,1), "copies preserve source, destination and transfer size");

    a->gpu.textag[0] = b->gpu.textag[0] = 99; a->gpu.clut_valid = b->gpu.clut_valid = 1;
    n = r.n; gp1(a, b, 0x01000000);
    CHECK(r.n == n + 1 && r.op[n].type == RENDER_CLEAR && r.op[n].p[0] == 1 && a->gpu.textag[0] == 99 && !a->gpu.clut_valid, "GP1 command reset invalidates only the CLUT cache");
    n = r.n; GP0(0xa0000000, V(10,10), V(3,1), 0x22221111); gp1(a, b, 0x01000000);
    CHECK(r.n == n + 3 && r.op[n].type == RENDER_UPLOAD && r.op[n+1].type == RENDER_UPLOAD && r.op[n+2].type == RENDER_CLEAR && !a->gpu.xfer,
          "aborting an upload retains received pixels and emits no unreceived tail");
    n = r.n; GP0(0x01000000);
    CHECK(r.n == n + 1 && r.op[n].type == RENDER_CLEAR && !r.op[n].p[0] && !a->gpu.textag[0], "GP0 cache clear invalidates texture tags and CLUT");
    n = r.n; GP0(0x200000ff, V(1,1)); gp1(a, b, 0);
    CHECK(r.n == n + 1 && r.op[n].type == RENDER_CLEAR && !r.op[n].p[0] && !a->gpu.n, "GP1 full reset aborts partial packets and clears both cache validities");
    a->gpu.texcache[0][0] = b->gpu.texcache[0][0] = 0x9876; a->gpu.clutcache[0] = b->gpu.clutcache[0] = 0x1234;
    uint16_t saved = a->gpu.vram[0]; n = r.n; gpu_reset(a); gpu_reset(b);
    CHECK(r.n == n + 2 && r.op[n].type == RENDER_RESET && r.op[n+1].type == RENDER_CLEAR, "machine GPU reset emits reset before GP1 reset");
    CHECK(a->gpu.vram[0] == saved && !a->gpu.texcache[0][0] && !a->gpu.clutcache[0] && a->on_render == record && a->render_ctx == &r, "GPU reset preserves VRAM and its host hook while clearing all cache contents");
    CHECK(!memcmp(&a->gpu, &b->gpu, sizeof a->gpu) && a->i_stat == b->i_stat && a->next_event == b->next_event, "attaching a consumer leaves all software GPU state unchanged");
    n = r.n; gpu_gp0(other, 0x020000ff); gpu_gp0(other, 0); gpu_gp0(other, V(16,1));
    CHECK(separate.n == 1 && separate.op[0].type == RENDER_FILL && r.n == n && a->gpu.vram[0] == saved, "another machine owns an independent operation stream and VRAM");
    a->on_render = NULL; GP0(0x020000ff, 0, V(16,1));
    CHECK(r.n == n && !memcmp(&a->gpu, &b->gpu, sizeof a->gpu), "detaching the consumer restores ordinary reference execution");
    ps1_free(a); ps1_free(b); ps1_free(other);
    printf("render: %d checks, %d failed\n", checks, fails); return fails != 0;
}
