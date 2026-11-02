// Synthetic texture pages through real GP0 packets (plan.md, phase 2). The disc smoke reached textured commands but the old renderer discarded all of them.
// psx-spx, GPU: Render Polygon/Rectangle Commands, Rendering Attributes E1h/E2h/E6h, Texture Bitmaps/Palettes, Clear Cache, GPU Texture Caching, Dithering, Semi-transparency and Modulation:
// https://psx-spx.consoledev.net/graphicsprocessingunitgpu/ . No game files or GPU are needed
#include "ps1.h"
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static ps1 *m;
#define V(x,y) ((uint32_t)(y) << 16 | (uint16_t)(x))
#define UV(u,v,attr) ((uint32_t)(attr) << 16 | (uint32_t)(v) << 8 | (u))
#define GP0(...) do { const uint32_t w_[] = {__VA_ARGS__}; for (unsigned i_ = 0; i_ < sizeof w_ / sizeof *w_; i_++) gpu_gp0(m, w_[i_]); } while (0)
#define PX(x,y) m->gpu.vram[(y) * VRAM_W + (x)]
static void sprite(uint32_t cmd, int x, int y, int u, int v, uint32_t clut, int w, int h) { GP0(cmd, V(x,y), UV(u,v,clut), V(w,h)); }
static void reset(void) { gpu_reset(m); memset(m->gpu.vram, 0, sizeof m->gpu.vram); GP0(0xe3000000, 0xe4000000 | 1023 | 511 << 10); }

static void caches(void) {   // GPU Texture Caching: persistent CLUT and 256 eight-byte entries, replaced by matching positions in different cache blocks
    reset(); unsigned clut = 20 | 300 << 6, other = 21 | 300 << 6;
    PX(0,0) = 1; PX(321,300) = 0x1f; sprite(0x65000000,400,0,0,0,clut,1,1);
    PX(321,300) = 0x3e0; sprite(0x65000000,401,0,0,0,clut,1,1);
    CHECK(PX(400,0) == 0x1f && PX(401,0) == 0x1f, "same CLUT index retains cached colors after VRAM mutation");
    GP0(0x01000000); sprite(0x65000000,402,0,0,0,clut,1,1);
    CHECK(PX(402,0) == 0x3e0, "GP0(01h) reloads modified CLUT colors");
    PX(321,300) = 0x7fff; PX(337,300) = 0x7c00;
    sprite(0x65000000,403,0,0,0,other,1,1); sprite(0x65000000,404,0,0,0,clut,1,1);
    CHECK(PX(403,0) == 0x7c00 && PX(404,0) == 0x7fff, "changing CLUT index invalidates its cached palette, including on return to the old index");
    PX(0,0) = 2; PX(338,300) = 0x421; sprite(0x65000000,405,0,0,0,other,1,1);
    CHECK(PX(405,0) == 0x7c00, "changing CLUT index leaves already cached texture indices intact");
    GP0(0x01000000); sprite(0x65000000,406,0,0,0,other,1,1);
    CHECK(PX(406,0) == 0x421, "GP0(01h) invalidates both cached texture indices and palette");

    for (int depth = 0; depth < 3; depth++) {
        reset(); GP0(0xe1000000 | (uint32_t)depth << 7); PX(321,300) = 0x1f; PX(322,300) = 0x3e0;
        uint16_t red = depth == 0 ? 0x1111 : depth == 1 ? 0x0101 : 0x1f, green = depth == 0 ? 0x2222 : depth == 1 ? 0x0202 : 0x3e0;
        int block = depth == 0 ? 64 : 32, word = depth == 2 ? 32 : 16;
        PX(0,0) = PX(1,0) = red; PX(word,0) = red; sprite(0x65000000,400,4,0,0,clut,1,1);
        PX(0,0) = green; sprite(0x65000000,401,4,0,0,clut,1,1);
        CHECK(PX(400,4) == 0x1f && PX(401,4) == 0x1f, "cached 4/8/15-bit texture data remains stale after VRAM mutation");
        PX(1,0) = green; sprite(0x65000000,407,4,depth == 0 ? 4 : depth == 1 ? 2 : 1,0,clut,1,1);
        CHECK(PX(407,4) == 0x1f, "a cache entry snapshots all four VRAM halfwords on its first access");
        PX(4,0) = red; sprite(0x65000000,408,4,depth == 0 ? 16 : depth == 1 ? 8 : 4,0,clut,1,1); sprite(0x65000000,409,4,0,0,clut,1,1);
        CHECK(PX(408,4) == 0x1f && PX(409,4) == 0x1f, "a miss in another cache entry preserves existing cached data");
        sprite(0x65000000,402,4,block,0,clut,1,1); sprite(0x65000000,403,4,0,0,clut,1,1);
        CHECK(PX(402,4) == 0x1f && PX(403,4) == 0x3e0, "horizontal cache-block collision replaces its entry for every texture depth");
        PX(0,0) = red; GP0(0x01000000); sprite(0x65000000,404,4,0,0,clut,1,1);
        CHECK(PX(404,4) == 0x1f, "GP0(01h) refreshes stale data at every texture depth");
        int rows = depth == 2 ? 32 : 64; PX(0,rows) = red; PX(0,0) = green;
        sprite(0x65000000,405,4,0,rows,clut,1,1); sprite(0x65000000,406,4,0,0,clut,1,1);
        CHECK(PX(405,4) == 0x1f && PX(406,4) == 0x3e0, "vertical cache-block collision replaces its entry for every texture depth");
    }
}

int main(void) {
    m = ps1_new(); if (!m) return 1;
    reset();
    unsigned clut = 20 | 300 << 6;
    PX(0,0) = 0x4321; PX(321,300) = 0x001f; PX(322,300) = 0x03e0; PX(323,300) = 0x7c00; PX(324,300) = 0x7fff;
    sprite(0x65000000,400,0,0,0,clut,4,1);
    CHECK(PX(400,0) == 0x1f && PX(401,0) == 0x3e0 && PX(402,0) == 0x7c00 && PX(403,0) == 0x7fff, "4-bit texels select low to high nibbles from a CLUT");
    PX(0,0) = 0; PX(320,300) = 0x421;
    GP0(0x01000000);   // fixture updates must explicitly invalidate texture and CLUT caches before expecting the new data
    sprite(0x65000000,400,1,0,0,clut,1,1);
    CHECK(PX(400,1) == 0x421, "palette index zero is visible when its CLUT color is nonzero");
    PX(320,300) = 0; PX(400,2) = 0x1234;
    GP0(0x01000000);
    sprite(0x65000000,400,2,0,0,clut,1,1);
    CHECK(PX(400,2) == 0x1234, "zero CLUT color is fully transparent");
    GP0(0xe1000080); PX(0,0) = 0x0201;
    GP0(0x01000000);
    sprite(0x65000000,400,4,0,0,clut,2,1);
    CHECK(PX(400,4) == 0x1f && PX(401,4) == 0x3e0, "8-bit texels select low then high byte");
    PX(0,0) = 255; PX(239,301) = 0x1234;
    GP0(0x01000000);
    sprite(0x65000000,400,5,0,0,63 | 301 << 6,1,1);
    CHECK(PX(400,5) == 0x1234, "CLUT lookup wraps horizontally within its VRAM row");

    GP0(0xe1000111); PX(64,256) = 0x1234; PX(65,256) = 0x5678; PX(64,257) = 0x421;
    GP0(0x01000000);
    sprite(0x65000000,400,8,0,0,0,2,2);
    CHECK(PX(400,8) == 0x1234 && PX(401,8) == 0x5678 && PX(400,9) == 0x421, "15-bit texture pages use X in 64-word and Y in 256-line steps");
    GP0(0xe1000191); sprite(0x65000000,400,10,0,0,0,1,1);
    CHECK(PX(400,10) == 0x1234, "reserved texture depth 3 acts as 15-bit depth");
    GP0(0xe100010f); PX(960,0) = 0x1f; PX(63,0) = 0x7c00;
    GP0(0x01000000);
    sprite(0x65000000,400,12,127,0,0,2,1);
    CHECK(PX(400,12) == 0x7c00 && PX(401,12) == PX(64,0), "direct-color page addressing wraps VRAM X");
    GP0(0xe1000100); PX(255,0) = 0x1f; PX(0,0) = 0x3e0;
    GP0(0x01000000);
    sprite(0x65000000,400,14,255,0,0,2,1);
    CHECK(PX(400,14) == 0x1f && PX(401,14) == 0x3e0, "rectangle UV wraps at 256 texels");
    PX(8,16) = 0x4321; GP0(0xe2000000 | 1 | 2 << 5 | 1 << 10 | 2 << 15);
    GP0(0x01000000);
    sprite(0x65000000,400,16,0,0,0,1,1);
    CHECK(PX(400,16) == 0x4321, "texture window replaces masked UV bits before sampling");
    GP0(0xe2000000, 0xe1003100); PX(6,5) = 0x1f; PX(5,5) = 0x3e0; PX(6,4) = 0x7c00; PX(5,4) = 0x7fff;
    GP0(0x01000000);
    sprite(0x65000000,400,20,5,5,0,2,2);
    CHECK(PX(400,20) == 0x1f && PX(401,20) == 0x3e0 && PX(400,21) == 0x7c00 && PX(401,21) == 0x7fff,
          "rectangle X flip runs U down from u0+1, Y flip runs V down from v0 (ps1-tests gpu/texture-flip)");
    GP0(0xe1000100, 0xe3000000 | 402 | 24 << 10, 0xe4000000 | 403 | 24 << 10); PX(2,0) = 0x1f; PX(3,0) = 0x3e0;
    GP0(0x01000000);
    sprite(0x65000000,400,24,0,0,0,4,1);
    CHECK(!PX(400,24) && PX(402,24) == 0x1f && PX(403,24) == 0x3e0, "clipping does not reset the sprite texture origin");
    GP0(0xe3000000, 0xe4000000 | 1023 | 511 << 10);

    PX(0,0) = 0x421;
    GP0(0x01000000);
    sprite(0x64808080,400,28,0,0,0,1,1); sprite(0x64404040,401,28,0,0,0,1,1); sprite(0x64ffffff,402,28,0,0,0,1,1);
    CHECK(PX(400,28) == 0x421 && PX(401,28) == 0 && PX(402,28) == 0x421, "modulation uses 80h as neutral and quantizes only after multiplication");
    PX(0,0) = 0x7fff; GP0(0x01000000); sprite(0x64ffffff,403,28,0,0,0,1,1);
    CHECK(PX(403,28) == 0x7fff, "modulation saturates channel brightness");
    PX(0,0) = 0x421; GP0(0x01000000, 0xe1000300);
    sprite(0x64808080,400,32,0,0,0,2,1); PX(1,0) = 0x421;
    GP0(0x01000000);
    sprite(0x64808080,400,32,0,0,0,2,1);
    CHECK(PX(400,32) == 0x421 && PX(401,32) == 0x421, "rectangle modulation never dithers");

    GP0(0xe1000120); PX(0,0) = 0x800a; PX(1,0) = 10; PX(400,36) = PX(401,36) = 8;
    GP0(0x01000000);
    sprite(0x67000000,400,36,0,0,0,2,1);
    CHECK(PX(400,36) == 0x8012 && PX(401,36) == 10, "semi-transparent command blends only texels with STP, preserving STP in destination");
    GP0(0xe1000100); PX(0,0) = 0x8000; PX(400,37) = 8;
    GP0(0x01000000);
    sprite(0x65000000,400,37,0,0,0,1,1);
    CHECK(PX(400,37) == 0x8000, "8000h is visible black, distinct from transparent 0000h");
    GP0(0xe6000002); PX(0,0) = 0x1f; GP0(0x01000000); sprite(0x65000000,400,37,0,0,0,1,1);
    CHECK(PX(400,37) == 0x8000, "mask check protects a previous textured pixel's STP");
    GP0(0xe6000001); sprite(0x65000000,401,37,0,0,0,1,1);
    CHECK(PX(401,37) == 0x801f, "force-mask applies to textured pixels");
    GP0(0xe6000000);

    for (int y=0; y<8; y++) for (int x=0; x<8; x++) PX(x,y) = (uint16_t)(1+x+y*8);
    GP0(0x01000000);
    GP0(0x25000000,V(400,100),UV(0,0,0),V(404,100),UV(4,0,0x100),V(400,104),UV(0,4,0));
    CHECK(PX(400,100) == 1 && PX(402,101) == PX(2,1) && !PX(404,100) && !PX(400,104), "raw triangle samples affine UV and excludes right/bottom edges");
    GP0(0xe1003100,0x2d000000,V(410,100),UV(0,0,0),V(414,100),UV(4,0,0x100),V(410,104),UV(0,4,0),V(414,104),UV(4,4,0));
    int count=0; for(int y=100;y<104;y++) for(int x=410;x<414;x++) count += PX(x,y) != 0;
    CHECK(count == 16 && PX(412,102) == PX(2,2), "textured quads join without cracks and ignore rectangle UV-flip flags");
    PX(0,0) = PX(1,0) = PX(2,0) = PX(0,1) = 31;
    GP0(0x01000000);
    GP0(0xe1000100,0x34808080,V(400,112),UV(0,0,0),0,V(404,112),UV(0,0,0x100),0x808080,V(400,116),UV(0,0,0));
    CHECK(PX(400,112) == 31 && PX(402,112) == 15, "Gouraud colors modulate texture after affine interpolation");
    PX(0,0)=0x421;
    GP0(0x01000000);
    GP0(0xe1000300,0x24808080,V(400,128),UV(0,0,0),V(404,128),UV(0,0,0x100),V(400,132),UV(0,0,0));
    CHECK(PX(400,128) == 0 && PX(401,128) == 0x421, "flat modulated polygons dither according to screen coordinates");
    CHECK((m->gpu.texpage & 0x200) && m->red == RED_GPU_TIMING, "a polygon's page keeps E1h's dithering, and only unmodelled drawing time reports a gap");
    caches();
    ps1_free(m); printf("texture: %d checks, %d failed\n", checks,fails); return fails != 0;
}
