// DMA (dma.c; psx-spx, DMA Channels and GPU Depth Ordering): OTC builds an empty ordering table (psx-spx's example), a channel waits for its DPCR enable, the GPU
// channel follows a linked list of packets and moves blocks to and from VRAM, and DICR raises IRQ 3 on a completion it enables, until the flag is written back
#include "ps1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static ps1 *m;
static void start(int ch, uint32_t madr, uint32_t bcr, uint32_t chcr) {
    mem_write32(m, 0x1f801080 + 16 * (uint32_t)ch, madr); mem_write32(m, 0x1f801084 + 16 * (uint32_t)ch, bcr); mem_write32(m, 0x1f801088 + 16 * (uint32_t)ch, chcr);
}
static uint32_t chcr(int ch) { return mem_read32(m, 0x1f801088 + 16 * (uint32_t)ch); }
static uint16_t px(int x, int y) { return m->gpu.vram[y * VRAM_W + x]; }

int main(void) {
    m = ps1_new();
    if (!m) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }
    enum { DPCR = 0x1f8010f0, DICR = 0x1f8010f4 };
    CHECK(mem_read32(m, DPCR) == 0x07654321, "DPCR after reset");

    start(6, 0x8012300c, 4, 0x11000002);
    CHECK(chcr(6) & 0x01000000 && mem_read32(m, 0x80123000) == 0, "a channel DPCR does not enable waits");
    mem_write32(m, DPCR, 0x07654321 | 1u << 27);
    CHECK(mem_read32(m, 0x80123000) == 0x00ffffff && mem_read32(m, 0x80123004) == 0x00123000 && mem_read32(m, 0x80123008) == 0x00123004 &&
          mem_read32(m, 0x8012300c) == 0x00123008, "OTC: each entry points to the one before, the first holds the end code");
    CHECK(!(chcr(6) & 0x11000000), "OTC: start and trigger bits clear when done");

    mem_write32(m, DPCR, mem_read32(m, DPCR) | 1u << 11);
    mem_write32(m, DICR, 0x00840000);   // IRQ on channel 2's completion, master enable
    const uint32_t list[][4] = {{3u << 24 | 0x010100, 0x020000ff, 0x00000000, 0x00010010}, {0x010200}, {1u << 24 | 0xffffff, 0xe1000123}};
    const uint32_t at[] = {0x80010000, 0x80010100, 0x80010200};
    for (int p = 0; p < 3; p++) for (uint32_t i = 0; i <= list[p][0] >> 24; i++) mem_write32(m, at[p] + 4 * i, list[p][i]);
    start(2, 0x010000, 0, 0x01000401);
    CHECK(px(0, 0) == 0x001f && px(15, 0) == 0x001f && (gpu_stat(m) & 0x7ff) == 0x123, "GPU linked list: every packet's words reach GP0, an empty one too");
    CHECK(mem_read32(m, 0x1f8010a0) == 0xffffff && !(chcr(2) & 0x01000000), "GPU linked list: MADR at the end code, start bit clear");
    CHECK(mem_read32(m, DICR) >> 24 == 0x84 && m->i_stat & 1u << IRQ_DMA, "DICR: channel 2's flag and the master flag set, IRQ 3 raised");
    mem_write32(m, DICR, 0x04840000);
    CHECK(mem_read32(m, DICR) == 0x00840000, "DICR: writing a flag's 1 acknowledges it, and the master flag falls");

    gpu_gp0(m, 0xa0000000); gpu_gp0(m, 0x00000020); gpu_gp0(m, 0x00020002);
    mem_write32(m, 0x80011000, 0x7fff0001); mem_write32(m, 0x80011004, 0x00030002);
    start(2, 0x011000, 1u << 16 | 2, 0x01000201);
    CHECK(px(32, 0) == 1 && px(33, 0) == 0x7fff && px(32, 1) == 2 && px(33, 1) == 3, "GPU block transfer to VRAM after GP0(A0h)");
    CHECK(mem_read32(m, 0x1f8010a0) == 0x011008 && mem_read32(m, 0x1f8010a4) >> 16 == 0, "sync mode 1: MADR moves past the blocks, BCR's count reaches 0");
    gpu_gp0(m, 0xc0000000); gpu_gp0(m, 0x00000020); gpu_gp0(m, 0x00020002);
    start(2, 0x012000, 1u << 16 | 2, 0x01000200);
    CHECK(mem_read32(m, 0x80012000) == 0x7fff0001 && mem_read32(m, 0x80012004) == 0x00030002 && !(gpu_stat(m) >> 27 & 1), "GPU block transfer from VRAM after GP0(C0h)");

    printf("dma: %d checks, %d failed\n", checks, fails);
    ps1_free(m);
    return fails != 0;
}
