// Synthetic streaming video blocks (plan.md, phase 2); psx-spx, Macroblock Decoder: command/status ports, tables, RLE, custom two-pass IDCT, monochrome/RGB packing and
// DMA1 rearrangement. Coefficients and scale matrices are constructed here; no movie/game data. Flat custom scale row 4000h makes DC*2 become DC/8 uniformly in both passes
#include "ps1.h"
#include "mdec.h"
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static ps1 *m;
static void put(uint32_t v) { mdec_write(m, 0, v); }
static uint32_t status(void) { return mdec_read(m, 4); }
static void quant(uint8_t v, int color) { put(0x40000000 | (uint32_t)color); for (int i = 0; i < (color ? 32 : 16); i++) put((uint32_t)v * 0x01010101); }
static void scale(int diagonal) {
    put(0x60000000);
    for (int i = 0; i < 32; i++) {
        int p = i * 2; uint32_t a = diagonal ? (p / 8 == p % 8 ? 0x4007 : 0) : (p < 8 ? 0x4000 : 0);
        uint32_t b = diagonal ? ((p + 1) / 8 == (p + 1) % 8 ? 0x4007 : 0) : (p + 1 < 8 ? 0x4000 : 0); put(a | b << 16);
    }
}
static void setup(int diagonal) { mdec_reset(m); quant(8, 1); scale(diagonal); }
static void dc(int value) { put((uint32_t)(value & 1023) | 0xfe000000); }
static int drain(uint32_t expected, int words) { int good = 1; for (int i = 0; i < words; i++) { if (status() >> 31 || mdec_read(m, 0) != expected) good = 0; } return good && (status() >> 31); }

static void colored(int depth) {
    setup(0); put(0x20000006 | (uint32_t)depth << 27 | 1u << 25); dc(0); dc(0); dc(0); dc(64); dc(128); dc(192);
}

static void dma(int ch, uint32_t addr, uint32_t bcr) {
    uint32_t base = 0x1f801080 + (uint32_t)ch * 16;
    mem_write32(m, base, addr); mem_write32(m, base + 4, bcr); mem_write32(m, base + 8, 0x01000200 | (uint32_t)!ch);
}
static void dma_enable(void) { dma_reset(m); mem_write32(m, 0x1f8010f0, m->dpcr | 0x88); mem_write32(m, 0x1f8010f4, 0x00830000); }

int main(void) {
    m = ps1_new(); if (!m) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }
    CHECK(status() == 0x80040000, "reset status: output empty, current block Y/Cr, counter zero");
    put(0x1e001234); CHECK(status() == (0x80040000 | 0x07800000 | 0x1234), "no-op copies command format/count bits without receiving parameters");
    put(0xfe005678); CHECK((status() & 0x6000ffff) == 0x5678, "invalid command behaves like no-op without setting busy/full");
    mdec_reset(m); put(0x40000001); CHECK((status() & 0x2000ffff) == 0x2000001f, "color quant table expects 32 parameter words");
    put(0x04030201); CHECK(m->mdec.quant[0][0] == 1 && m->mdec.quant[0][3] == 4 && (status() & 0xffff) == 30, "quant bytes arrive little-endian and decrement count on receive");
    for (int i = 1; i < 32; i++) put(0x08080808);
    CHECK(!(status() & (1u << 29)) && (status() & 0xffff) == 0xffff && m->mdec.quant[1][63] == 8, "quant upload finishes after both tables");
    scale(1); CHECK(m->mdec.scale[0] == 0x4007 && m->mdec.scale[1] == 0 && m->mdec.scale[63] == 0x4007 && !(status() & (1u << 29)), "custom signed scale table is uploaded in halfword order");

    setup(0); mdec_write(m, 4, 0x60000000); put(0x28000001); dc(0);
    CHECK((status() & 0x0f070000) == 0x0a040000 && (m->red & RED_MDEC), "8-bit output reports block/depth, output DREQ, and explicit rounding/timing gap");
    CHECK(!mdec_dma_offset(m) && drain(0x80808080, 16), "8-bit unsigned zero DCT gives an 8x8 mid-gray bitmap");
    setup(0); put(0x20000001); dc(0); CHECK(drain(0x88888888, 8), "4-bit mode packs two upper-nibble luminances per byte");
    setup(0); put(0x28000001); dc(72); CHECK(drain(0x89898989, 16), "8-bit DC/8 luminance 89h");
    setup(0); put(0x20000001); dc(72); CHECK(drain(0x99999999, 8), "4-bit output rounds the 8-bit value: 89h gives 9, not 8 (ps1-tests mdec/4bit)");
    setup(0); put(0x2c000001); dc(0); CHECK(drain(0, 16), "signed output omits unsigned luminance bias");
    setup(0); put(0x28000001); dc(128); CHECK(drain(0x90909090, 16), "custom flat scale table gives uniform DC/8 luminance");
    setup(0); put(0x28000001); dc(-128); CHECK(drain(0x70707070, 16), "negative signed10 DC is decoded before unsigned bias");
    setup(0); put(0x28000002); put(0x0000fe00); put(0xfe00fe00); CHECK(drain(0x80808080, 16), "FE00 padding before/after a block is skipped without producing extra blocks");
    setup(0); put(0x28000020); for (int i = 0; i < 32; i++) put(0);
    CHECK(drain(0x80808080, 16) && !m->mdec.active, "a block ends once index 63 is written, with no end code after it (ps1-tests mdec/4bit)");

    setup(1); put(0x28000002); put(0x04800000);
    CHECK(m->mdec.coeff[0] == 0 && m->mdec.coeff[2] == 256 && m->mdec.active, "q_scale=0 ignores quant table, doubles AC and uses linear run positions");
    put(0xfe00fe00); uint32_t first = mdec_read(m, 0);
    CHECK(first == 0x80908080 && drain(0x80808080, 15), "custom diagonal IDCT preserves spatial coefficient position and discards scale low3 bits");
    setup(1); put(0x28000002); put(0x04102000);
    CHECK(m->mdec.coeff[8] == 128 && m->mdec.coeff[2] == 0, "nonzero q_scale applies quant scaling and inverse zigzag placement");
    put(0xfe00fe00); int good = 1;
    for (int i = 0; i < 16; i++) if (mdec_read(m, 0) != (i == 2 ? 0x80808088 : 0x80808080)) good = 0;
    CHECK(good, "quantized AC scan2 lands in natural row1,column0 after IDCT");
    setup(1); quant(255, 0); put(0x28000002); put(0x0200fdff);
    CHECK(m->mdec.coeff[0] == 1023 && m->mdec.coeff[1] == -1024, "RLE quantized coefficients saturate to signed11 limits");
    mdec_write(m, 4, 0x80000000); CHECK(status() == 0x80040000 && !m->mdec.in_n && !m->mdec.out_n && !m->mdec.active, "control reset aborts partial RLE and clears both queues/status");

    setup(0); put(0x30000006); dc(128); dc(-128); for (int i = 0; i < 4; i++) dc(0); good = 1;
    // Flat IDCT gives Cr=+16, Cb=-16, Y=0: published coefficients give signed RGB=(22,-6,-29), then unsigned bias gives (150,122,99)
    for (int i = 0; i < 192; i++) { uint32_t word = mdec_read(m, 0), expected = 0; static const uint8_t channel[3] = {150,122,99}; for (int j = 0; j < 4; j++) expected |= (uint32_t)channel[(i * 4 + j) % 3] << (j * 8); if (word != expected) good = 0; }
    CHECK(good, "nonzero chroma converts Cr/Cb into independent RGB24 channels with signed arithmetic");
    setup(0); put(0x3c000006); dc(128); dc(-128); for (int i = 0; i < 4; i++) dc(0);
    CHECK(drain(0x73e273e2, 128), "signed RGB15 omits bias before truncating channels to five bits");
    setup(0); put(0x60000000); for (int i = 0; i < 32; i++) put(0x7fff7fff);
    put(0x28000021); for (int i = 0; i < 32; i++) put(0x01ff01ff); put(0xfe00fe00);
    // 64 coefficients of 1022: pass 1 rounds to 4087, pass 2 to 16344; signed9 wraps to -40, which becomes 88 after unsigned bias
    CHECK(drain(0x58585858, 16), "monochrome wraps IDCT output to signed9 before signed8 saturation and bias");
    colored(3); CHECK((status() >> 16 & 7) == 0, "color output status names first Y block, rather than a later input block");
    uint16_t image15[256] = {0}; uint8_t seen15[128] = {0}; good = 1;
    for (int i = 0; i < 128; i++) {
        int offset = i * 4 + mdec_dma_offset(m); if (offset < 0 || offset >= 512 || offset % 4 || status() >> 31) { good = 0; break; }
        if (seen15[offset / 4]++) good = 0;
        uint32_t word = mdec_dma_read(m); memcpy((uint8_t *)image15 + offset, &word, 4);
    }
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int shade = 16 + (x / 8 + y / 8 * 2); uint16_t expected = (uint16_t)(0x8000 | shade | shade << 5 | shade << 10); if (image15[y * 16 + x] != expected) good = 0; }
    CHECK(good && status() >> 31, "15-bit output sets STP and DMA1 rearranges four 8x8 quadrants into 16x16 exactly once");
    colored(2); uint8_t image24[768] = {0}, seen24[192] = {0}; good = 1;
    for (int i = 0; i < 192; i++) {
        int offset = i * 4 + mdec_dma_offset(m); if (offset < 0 || offset >= 768 || offset % 4 || status() >> 31) { good = 0; break; }
        if (seen24[offset / 4]++) good = 0;
        uint32_t word = mdec_dma_read(m); memcpy(image24 + offset, &word, 4);
    }
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) for (int c = 0; c < 3; c++) if (image24[(y * 16 + x) * 3 + c] != 128 + (x / 8 + y / 8 * 2) * 8) good = 0;
    CHECK(good && status() >> 31, "24-bit RGB bytes remain correctly ordered across 48-word blocks and 32-word DMA slices");

    setup(0); mdec_write(m, 4, 0x60000000); put(0x28000028); int sent = 0, received = 0;
    while (sent < 40 && mdec_dma_ready(m, 0)) { dc(0); sent++; }
    CHECK(sent < 40 && m->mdec.out_n == 64 && m->mdec.in_n >= 63 && (status() & (1u << 30)) && !mdec_dma_ready(m, 0), "backpressure fills bounded output and 32-word input queues, then lowers input DREQ");
    good = 1;
    for (int guard = 0; guard < 10000 && (sent < 40 || !(status() >> 31)); guard++) {
        if (mdec_dma_ready(m, 1)) { if (mdec_dma_read(m) != 0x80808080) good = 0; received++; }
        if (sent < 40 && mdec_dma_ready(m, 0)) { dc(0); sent++; }
    }
    CHECK(good && sent == 40 && received == 640 && !(status() & (1u << 29)), "interleaved input/output resumes a blocked decoder without losing or duplicating any words");
    setup(0); dma_enable(); mem_write32(m, 0x1f801824, 0x60000000); mem_write32(m, 0x1f801820, 0x28000040);
    for (int i = 0; i < 64; i++) mem_write32(m, 0x80010000 + (uint32_t)i * 4, 0xfe000000);
    dma(0, 0x10000, 2u << 16 | 32);
    CHECK((m->dma[0].chcr & 0x01000000) && m->dma[0].left && !(m->dicr & (1u << 24)), "DMA0 stays busy without completion IRQ when decoder queues fill before DMA1 starts");
    dma(1, 0x12000, 32u << 16 | 32); good = 1;
    for (int i = 0; i < 1024; i++) if (mem_read32(m, 0x80012000 + (uint32_t)i * 4) != 0x80808080) good = 0;
    CHECK(good && !(m->dma[0].chcr & 0x01000000) && !(m->dma[1].chcr & 0x01000000) && !m->dma[0].left && !m->dma[1].left, "starting DMA1 resumes both channels through a stream larger than both FIFO capacities");
    CHECK(m->dma[0].madr == 0x10100 && m->dma[1].madr == 0x13000 && (m->dicr & 0x83000000) == 0x83000000 && (m->i_stat & (1u << IRQ_DMA)), "DMA counters/addresses and both DICR completions reach their final values");
    setup(0); dma_enable(); mem_write32(m, 0x1f801820, 0x28000001); mem_write32(m, 0x80010000, 0xfe000000); dma(0, 0x10000, 1u << 16 | 1);
    CHECK(m->dma[0].active && m->dma[0].left == 1 && m->mdec.remaining == 1, "disabled input DREQ keeps DMA0 pending");
    mem_write32(m, 0x1f801824, 0x40000000); dma(1, 0x12000, 1u << 16 | 16);
    CHECK(!m->dma[0].active && m->dma[1].active && m->dma[1].left == 16, "enabling only input DREQ completes DMA0 while disabled output keeps DMA1 pending");
    mem_write32(m, 0x1f801824, 0x60000000); CHECK(!m->dma[1].active && mem_read32(m, 0x80012000) == 0x80808080, "enabling output DREQ resumes waiting DMA1");
    setup(0); dma_enable(); mem_write32(m, 0x1f801824, 0x60000000); mem_write32(m, 0x1f801820, 0x30000006);
    for (int i = 0; i < 6; i++) mem_write32(m, 0x80010000 + (uint32_t)i * 4, 0xfe000000 | (uint32_t)(i < 2 ? 0 : (i - 2) * 64));
    dma(0, 0x10000, 1u << 16 | 6); dma(1, 0x12000, 6u << 16 | 32); good = 1;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) for (int c = 0; c < 3; c++)
        if (mem_read8(m, 0x80012000 + (uint32_t)((y * 16 + x) * 3 + c)) != 128 + (x / 8 + y / 8 * 2) * 8) good = 0;
    CHECK(good && !m->dma[0].active && !m->dma[1].active, "DMA1 uses output block metadata to rearrange actual RGB24 transfers in RAM");
    setup(0); dma_enable(); mem_write32(m, 0x1f801824, 0x60000000); mem_write32(m, 0x1f801820, 0x28000002);
    mem_write32(m, 0x80010000, 0xfe000000); mem_write32(m, 0x80010004, 0xfe000080); dma(1, 0x12000, 1u << 16 | 32);
    CHECK(m->dma[1].active && m->dma[1].left == 32, "DMA1 may start first and wait for compressed input");
    dma(0, 0x10000, 1u << 16 | 2); good = !m->dma[0].active && !m->dma[1].active;
    for (int i = 0; i < 32; i++) if (mem_read32(m, 0x80012000 + (uint32_t)i * 4) != (i < 16 ? 0x80808080 : 0x90909090)) good = 0;
    CHECK(good, "starting DMA0 wakes a previously blocked DMA1 and preserves multiple macroblock order");
    ps1 *other = ps1_new(); CHECK(other && mdec_read(other, 4) == 0x80040000 && !other->mdec.quant[0][0] && m->mdec.quant[0][0] == 8, "tables, queues and command state belong to each machine"); ps1_free(other);
    printf("mdec: %d checks, %d failed\n", checks, fails); ps1_free(m); return fails != 0;
}
