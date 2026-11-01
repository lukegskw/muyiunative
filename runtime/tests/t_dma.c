// DMA (dma.c; psx-spx, DMA Channels and GPU Depth Ordering): OTC builds an empty ordering table (psx-spx's example), a channel waits for its DPCR enable, the GPU
// channel follows a linked list of packets and moves blocks to and from VRAM. DICR requires both enables to latch a completion; existing flags survive mask/master
// changes until acknowledged, and can reassert IRQ3 when master enable rises (psx-spx, DMA Channels, 1F8010F4h)
#include "ps1.h"
#include <string.h>


static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static ps1 *m;
static void start(int ch, uint32_t madr, uint32_t bcr, uint32_t chcr) {
    mem_write32(m, 0x1f801080 + 16 * (uint32_t)ch, madr); mem_write32(m, 0x1f801084 + 16 * (uint32_t)ch, bcr); mem_write32(m, 0x1f801088 + 16 * (uint32_t)ch, chcr);
}
static uint32_t chcr(int ch) { return mem_read32(m, 0x1f801088 + 16 * (uint32_t)ch); }
static void advance(uint64_t cycles) { m->cycles += cycles; sched_run(m); }
static uint16_t px(int x, int y) { return m->gpu.vram[y * VRAM_W + x]; }
static void count_rectangles(void *ctx, const ps1_render_op *op) { if (op->type == RENDER_RECT) (*(unsigned *)ctx)++; }

static void drawing_waits(void) {
    gpu_reset(m); unsigned rectangles = 0; m->render_ctx = &rectangles; m->on_render = count_rectangles;
    const uint32_t setup[] = {0xe1000040, 0xe3000000, 0xe4000000 | 303 | 223 << 10, 0xe5000000, 0xe6000000};
    for (unsigned i = 0; i < sizeof setup / sizeof *setup; i++) gpu_gp0(m, setup[i]);
    // The measured failing loop requested this primitive >70,000 times/frame. The v2 hardware fit for a clipped 304x224 semi rectangle is 54,957 CPU cycles.
    mem_write32(m, 0x80020000, 0x03020000); mem_write32(m, 0x80020004, 0x62a0b0c0);
    mem_write32(m, 0x80020008, 0); mem_write32(m, 0x8002000c, 0x01000140);
    start(2, 0x020000, 0, 0x01000401);
    CHECK(m->dma[2].active && m->dma_next == NEVER && !rectangles, "GPU direction off holds a linked list without consuming its header");
    uint64_t issued = m->cycles;
    gpu_gp1(m, 0x04000002); advance(4);
    uint64_t done = m->gpu.draw_until;
    CHECK(rectangles == 1 && done == issued + 4 + 54957, "a semi rectangle occupies the GPU for its measured clipped drawing time");
    CHECK(!(gpu_stat(m) & ((1u << 26) | (1u << 28) | (1u << 25))), "GPUSTAT command-ready, DMA-ready and mode2 request stay low while drawing");
    advance(1000); mem_write32(m, 0x80021000, 0x12345678);
    CHECK(rectangles == 1 && mem_read32(m, 0x80021000) == 0x12345678 && m->dma[2].active, "the CPU runs during device backpressure without repeating the draw");
    advance(done - m->cycles - 1);
    CHECK(rectangles == 1, "a cyclic list cannot submit its next draw before the GPU is ready");
    advance(1);
    CHECK(rectangles == 2 && m->gpu.draw_until > done, "the same live list resumes on GPU readiness without skipping a command");
    advance(10);
    CHECK(m->dma[2].gpu_wait, "a drawing wait is distinguished from a packet's word-transfer deadline");
    gpu_gp1(m, 0x01000000); advance(1);
    CHECK(rectangles == 3, "resetting the command buffer wakes a list waiting on cancelled drawing occupancy");
    mem_write32(m, 0x1f8010a8, 0x00000401); advance(1000);
    CHECK(!m->dma[2].active && rectangles == 3 && m->dma_next == NEVER, "the guest can stop a live list during a drawing wait");
    CHECK(!(gpu_stat(m) & 1u << 28), "stopping DMA does not invent completion of GPU drawing");
    gpu_gp1(m, 0x01000000);
    CHECK((gpu_stat(m) & ((1u << 26) | (1u << 28))) == ((1u << 26) | (1u << 28)), "resetting the GPU command buffer releases drawing occupancy");
    // A command can span packets. An unfinished rectangle must receive its parameters rather than deadlock behind its own not-yet-started draw.
    mem_write32(m, 0x80022000, 0x01022010); mem_write32(m, 0x80022004, 0x600000ff);
    mem_write32(m, 0x80022010, 0x02ffffff); mem_write32(m, 0x80022014, 0x000a000a); mem_write32(m, 0x80022018, 0x00020002);
    start(2, 0x022000, 0, 0x01000401); advance(1);
    CHECK(m->gpu.n == 1, "a partial rectangle packet awaits parameters");
    gpu_gp1(m, 0x04000000); advance(20);
    CHECK(m->gpu.n == 1 && m->dma[2].active && m->dma_next == NEVER, "GP1 direction off pauses a partial command without losing it");
    gpu_gp1(m, 0x04000002); advance(10);
    CHECK(rectangles == 4 && px(10, 10) == 0x1f && !m->dma[2].active, "restoring DMA direction completes a split command and its transfer");
    gpu_gp1(m, 0x01000000);
    mem_write32(m, 0x80023000, 0x01ffffff); mem_write32(m, 0x80023004, 0);
    start(2, 0x023000, 0, 0x01000401); advance(1);
    CHECK(m->dma[2].active && m->dma[2].left, "a terminal packet waits for its last transferred word before DMA completion");
    uint32_t enabled = mem_read32(m, 0x1f8010f0);
    mem_write32(m, 0x1f8010f0, enabled & ~0x800u); gpu_gp1(m, 0x04000000);
    mem_write32(m, 0x1f8010f0, enabled); advance(2);
    CHECK(!m->dma[2].active && m->dma_next == NEVER && m->dma[2].madr == 0xffffff,
          "DPCR resume completes an already-transferred terminal packet even after GPU requests are disabled");
    m->on_render = NULL; m->render_ctx = NULL;
}

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
    gpu_gp1(m, 0x04000002);
    mem_write32(m, DICR, 0x00840000);   // IRQ on channel 2's completion, master enable
    const uint32_t list[][4] = {{3u << 24 | 0x010100, 0x020000ff, 0x00000000, 0x00010010}, {0x010200}, {1u << 24 | 0xffffff, 0xe1000123}};
    const uint32_t at[] = {0x80010000, 0x80010100, 0x80010200};
    for (int p = 0; p < 3; p++) for (uint32_t i = 0; i <= list[p][0] >> 24; i++) mem_write32(m, at[p] + 4 * i, list[p][i]);
    start(2, 0x010000, 0, 0x01000401);
    CHECK(chcr(2) & 0x01000000 && !px(0, 0), "GPU list start schedules work without drawing or advancing the CPU");
    advance(16);
    CHECK(px(0, 0) == 0x001f && px(15, 0) == 0x001f && (gpu_stat(m) & 0x7ff) == 0x123, "GPU linked list: every packet's words reach GP0, an empty one too");
    CHECK(mem_read32(m, 0x1f8010a0) == 0xffffff && !(chcr(2) & 0x01000000), "GPU linked list: MADR at the end code, start bit clear");
    CHECK(mem_read32(m, DICR) >> 24 == 0x84 && m->i_stat & 1u << IRQ_DMA, "DICR: channel 2's flag and the master flag set, IRQ 3 raised");
    mem_write32(m, DICR, 0x04840000);
    CHECK(mem_read32(m, DICR) == 0x00840000, "DICR: writing a flag's 1 acknowledges it, and the master flag falls");
    mem_write32(m, 0x1f801070, ~(1u << IRQ_DMA)); mem_write32(m, 0x80010300, 0x010300);
    start(2, 0x010300, 0, 0x01000401);
    CHECK(chcr(2) & 0x01000000 && mem_read32(m, DICR) == 0x00840000 && !(m->i_stat & 1u << IRQ_DMA),
          "a GPU list that comes back on itself never completes, nor raises its IRQ (ps1-tests dma/chain-looping)");
    mem_write32(m, 0x1f8010a8, 0);
    // A synthetic list containing a drawing command, not just empty headers: the CPU must get back to stop it, with no packet-count escape.
    mem_write32(m, 0x80010400, 0x03010410); mem_write32(m, 0x80010404, 0x0200ff00);
    mem_write32(m, 0x80010408, 0x00020000); mem_write32(m, 0x8001040c, 0x00010010); mem_write32(m, 0x80010410, 0x010400);
    uint64_t at_cycle = m->cycles;
    start(2, 0x010400, 0, 0x01000401);
    advance(20);
    CHECK(m->cycles == at_cycle + 20 && (chcr(2) & 0x01000000) && px(0, 2) == 0x03e0, "a looping list advances in guest time and stays busy");
    uint32_t enable = mem_read32(m, DPCR), next = mem_read32(m, 0x1f8010a0);
    mem_write32(m, DPCR, enable & ~0x800u); advance(20);
    CHECK(m->dma_next == NEVER && mem_read32(m, 0x1f8010a0) == next && (chcr(2) & 0x01000000), "DPCR disables service without completing or losing a running GPU list");
    mem_write32(m, DPCR, enable);
    mem_write32(m, 0x80010400, 0xffffff); advance(5);
    CHECK(!(chcr(2) & 0x01000000), "the CPU can break a running list by changing its next header");
    mem_write32(m, DICR, 0x04840000);
    start(2, 0x010410, 0, 0x01000401); mem_write32(m, 0x1f8010a8, 0); advance(20);
    CHECK(!(chcr(2) & 0x01000000) && m->dma_next == NEVER && mem_read32(m, DICR) == 0x00840000, "stopping DMA cancels its scheduled event without an IRQ");

    gpu_gp0(m, 0xa0000000); gpu_gp0(m, 0x00000020); gpu_gp0(m, 0x00020002);
    mem_write32(m, 0x80011000, 0x7fff0001); mem_write32(m, 0x80011004, 0x00030002);
    start(2, 0x011000, 1u << 16 | 2, 0x01000201);
    CHECK(px(32, 0) == 1 && px(33, 0) == 0x7fff && px(32, 1) == 2 && px(33, 1) == 3, "GPU block transfer to VRAM after GP0(A0h)");
    CHECK(mem_read32(m, 0x1f8010a0) == 0x011008 && mem_read32(m, 0x1f8010a4) >> 16 == 0, "sync mode 1: MADR moves past the blocks, BCR's count reaches 0");
    gpu_gp0(m, 0xc0000000); gpu_gp0(m, 0x00000020); gpu_gp0(m, 0x00020002);
    start(2, 0x012000, 1u << 16 | 2, 0x01000200);
    CHECK(mem_read32(m, 0x80012000) == 0x7fff0001 && mem_read32(m, 0x80012004) == 0x00030002 && !(gpu_stat(m) >> 27 & 1), "GPU block transfer from VRAM after GP0(C0h)");

    // Public DICR semantics: channel enable gates new completions, not an already latched flag's contribution to bit31
    mem_write32(m, DICR, 0x04000000); mem_write32(m, 0x1f801070, ~(1u << IRQ_DMA)); mem_write32(m, 0x80013000, 0);   // GP0 NOP
    mem_write32(m, DICR, 0x00040000); start(2, 0x013000, 1u << 16 | 1, 0x01000201);
    CHECK(mem_read32(m, DICR) == 0x00040000 && !(m->i_stat & (1u << IRQ_DMA)), "master-disabled completion does not latch a per-channel DICR flag");
    mem_write32(m, DICR, 0x00840000); start(2, 0x013000, 1u << 16 | 1, 0x01000201);
    CHECK(mem_read32(m, DICR) == 0x84840000 && (m->i_stat & (1u << IRQ_DMA)), "both enables latch channel completion and raise IRQ3");
    mem_write32(m, 0x1f801070, ~(1u << IRQ_DMA)); mem_write32(m, DICR, 0x00800000);
    CHECK(mem_read32(m, DICR) == 0x84800000 && !(m->i_stat & (1u << IRQ_DMA)), "disabling channel mask preserves its pending flag/master level without another IRQ edge");
    mem_write32(m, DICR, 0);
    CHECK(mem_read32(m, DICR) == 0x04000000 && !(m->i_stat & (1u << IRQ_DMA)), "master disable preserves latched completion while lowering bit31");
    mem_write32(m, DICR, 0x00800000);
    CHECK(mem_read32(m, DICR) == 0x84800000 && (m->i_stat & (1u << IRQ_DMA)), "master re-enable reasserts IRQ from retained completion even with channel mask disabled");
    mem_write32(m, DICR, 0x04800000);
    CHECK(mem_read32(m, DICR) == 0x00800000 && (m->i_stat & (1u << IRQ_DMA)), "flag acknowledgement lowers DICR master but I_STAT requires its own acknowledgement");
    mem_write32(m, 0x1f801070, ~(1u << IRQ_DMA)); start(2, 0x013000, 1u << 16 | 1, 0x01000201);
    CHECK(mem_read32(m, DICR) == 0x00800000 && !(m->i_stat & (1u << IRQ_DMA)), "channel-disabled completion does not latch a new flag with master enabled");
    mem_write32(m, DICR, 0x00008000);
    CHECK(mem_read32(m, DICR) == 0x80008000 && (m->i_stat & (1u << IRQ_DMA)), "force interrupt bypasses both enables");
    drawing_waits();
    printf("dma: %d checks, %d failed\n", checks, fails);
    ps1_free(m);
    return fails != 0;
}
