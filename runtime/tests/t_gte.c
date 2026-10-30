// Synthetic COP2 programs and fixed-point geometry (plan.md, phase 2). psx-spx, Geometry Transformation Engine: register extension/side effects; all documented commands;
// fixed numerical projection, lighting and interpolation examples; saturation flags and UNR boundary cases. No game programs, floating point or external GPU needed
#include "ps1.h"
#include "gte.h"
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
#define SF (1u << 19)
#define LM (1u << 10)
#define D (m->cpu.cop2d)
#define C (m->cpu.cop2c)
static ps1 *m;
static void fresh(void) { cpu_reset(m); m->cycles = 0; }
static void dw(unsigned r, uint32_t v) { gte_write(m, r, v, 0); }
static void cw(unsigned r, uint32_t v) { gte_write(m, r, v, 1); }
static uint32_t xy(int x, int y) { return (uint16_t)x | (uint32_t)(uint16_t)y << 16; }
static void vec(int n, int x, int y, int z) { dw((unsigned)n * 2, xy(x, y)); dw((unsigned)n * 2 + 1, (uint32_t)z); }
static void irs(int x, int y, int z) { dw(9, (uint32_t)x); dw(10, (uint32_t)y); dw(11, (uint32_t)z); }
static void identity(int base) { cw((unsigned)base, 4096); cw((unsigned)base + 1, 0); cw((unsigned)base + 2, 4096); cw((unsigned)base + 3, 0); cw((unsigned)base + 4, 4096); }
static int result(int x, int y, int z) { return (int32_t)D[25] == x && (int32_t)D[26] == y && (int32_t)D[27] == z; }
static void lighting(void) { fresh(); identity(8); identity(16); for (int i = 0; i < 3; i++) vec(i, 1024, 2048, 3072); dw(6, 0x20204080); dw(8, 2048); cw(21, 2048); cw(22, 1024); cw(23, 512); }

#define I(op,rs,rt,imm) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((uint32_t)(imm) & 0xffff))
#define M2(rs,rt,rd) (0x48000000u | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | (uint32_t)(rd) << 11)
static void program(const uint32_t *p, int n) { for (int i = 0; i < n; i++) mem_write32(m, 0x80001000 + (uint32_t)i * 4, p[i]); m->cpu.pc = 0x80001000; m->cpu.next_pc = 0x80001004; m->cpu.cop0[C0_SR] = 1u << 30; }
#define RUN(...) do { uint32_t p[] = {__VA_ARGS__}; int n = (int)(sizeof p / sizeof *p); program(p, n); while (n--) cpu_step(m); } while (0)

int main(void) {
    m = ps1_new(); if (!m) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }
    dw(1, 0x12348900); dw(9, 0x1234ffff); dw(16, 0xffff1234); cw(4, 0xabcd8000); cw(26, 0x1234f000);
    CHECK(gte_read(m, 1, 0) == 0xffff8900 && D[9] == 0xffffffff && D[16] == 0x1234, "vector registers sign-extend; SZ zero-extends");
    CHECK(gte_read(m, 4, 1) == 0xffff8000 && gte_read(m, 26, 1) == 0xfffff000, "matrix tail and H sign-extend on transfer, including H hardware bug");
    dw(12, 1); dw(13, 2); dw(14, 3); dw(15, 4);
    CHECK(D[12] == 2 && D[13] == 3 && D[14] == 4 && gte_read(m, 15, 0) == 4, "SXYP pushes SXY FIFO and reads back the SXY2 mirror");
    dw(14, 5); CHECK(D[12] == 2 && D[13] == 3 && gte_read(m, 15, 0) == 5, "direct SXY2 write does not push the FIFO");
    dw(28, 0x7fff); CHECK(D[9] == 3968 && D[10] == 3968 && D[11] == 3968 && gte_read(m, 28, 0) == 0x7fff, "IRGB expands 5-bit channels by 128");
    irs(-1, 4096, 128); dw(29, 0); CHECK(gte_read(m, 29, 0) == (31u << 5 | 1u << 10) && !C[31], "ORGB/IRGB clamp dynamically without flags; ORGB ignores writes");
    dw(30, 0); CHECK(D[31] == 32, "LZCS zero has 32 leading zeroes");
    dw(30, 0xffffffff); dw(31, 0); CHECK(D[31] == 32, "LZCS minus one has 32 leading ones; LZCR read-only");
    dw(30, 0xfff00000); CHECK(D[31] == 12, "LZCS counts leading sign bits");
    cw(31, 0xffffffff); CHECK(C[31] == 0xfffff000, "FLAG masks reserved bits and recomputes summary");
    cw(31, 1u << 22 | 1u << 21 | 1u << 12); CHECK(C[31] == (1u << 22 | 1u << 21 | 1u << 12), "IR3/color/IR0 flags do not imply FLAG31");

    fresh(); identity(0); vec(0, 100, -50, 1000); cw(26, 500); cw(27, 256); gte_command(m, SF | 1);
    CHECK(result(100, -50, 1000) && D[14] == xy(50, -25) && D[19] == 1000 && D[8] == 2048 && !C[31], "RTPS: identity, H/Z=1/2, screen (50,-25), depth IR0=2048");
    CHECK(m->cpu.gte_busy == 15 && m->cycles == 0, "RTPS runs in parallel for 15 cycles");
    CHECK(gte_read(m, 19, 0) == 1000 && m->cycles == 15, "reading a GTE register waits for command completion");
    gte_command(m, 6); uint64_t issued = m->cycles; dw(6, 0x20000000);
    CHECK(m->cycles == issued && (m->red & RED_GTE_TIMING), "writes during execution do not fabricate a stall and explicitly report the timing gap");
    uint64_t busy = m->cpu.gte_busy; gte_command(m, 6); CHECK(m->cycles == busy && m->cpu.gte_busy == busy + 8, "issuing a second command waits for the first");
    fresh(); identity(0); cw(26, 500); vec(0, 100, 0, 1000); vec(1, 0, 100, 1000); vec(2, -100, -100, 1000); dw(19, 77); gte_command(m, SF | 0x30);
    CHECK(D[12] == xy(50, 0) && D[13] == xy(0, 50) && D[14] == xy(-50, -50) && D[16] == 77 && D[17] == 1000 && D[18] == 1000 && D[19] == 1000, "RTPT pushes three screen/depth results in vertex order");
    fresh(); identity(0); vec(0, 1, 2, 10); cw(26, 1); gte_command(m, 1);
    CHECK(D[11] == 32767 && !(C[31] & (1u << 22)) && D[19] == 10, "RTPS sf=0 checks IR3 flag after fixed >>12 while storing unshifted IR3");
    fresh(); identity(0); vec(0, 0, 0, -1); gte_command(m, SF | LM | 1);
    CHECK(D[11] == 0 && !(C[31] & (1u << 22)) && (C[31] & (1u << 18)) && (C[31] & (1u << 17)), "RTPS IR3 lm=1 clamps negative Z without IR3 flag, but SZ and divide flag");
    fresh(); cw(7, 0x7f20); cw(26, 0xfe3f); cw(27, 1); gte_command(m, SF | 1);
    CHECK(D[24] == 0x1ffff && !(C[31] & (1u << 17)), "documented FE3Fh/7F20h UNR case clamps reciprocal without divide-overflow flag");
    fresh(); cw(7, 1); cw(26, 1); cw(27, 1); gte_command(m, SF | 1); CHECK(D[24] == 0x10000 && !(C[31] & (1u << 17)), "UNR smallest nonzero depth: 1/1");
    cw(26, 2); gte_command(m, SF | 1); CHECK(D[24] == 0x1ffff && (C[31] & (1u << 17)), "H==2*SZ sets near-plane divide overflow");
    fresh(); dw(12, xy(0, 0)); dw(13, xy(10, 0)); dw(14, xy(0, 10)); gte_command(m, 6);
    CHECK(D[24] == 100, "NCLIP returns twice a counterclockwise triangle's area");
    dw(13, xy(0, 10)); dw(14, xy(10, 0)); gte_command(m, 6); CHECK((int32_t)D[24] == -100, "NCLIP reverses sign for reversed winding");
    fresh(); for (int i = 0; i < 4; i++) dw((unsigned)(16 + i), (uint32_t)(i + 1) * 100); cw(29, 1365); cw(30, 1024); gte_command(m, 0x2d);
    CHECK(D[24] == 1228500 && D[7] == 299, "AVSZ3 uses only SZ1..3 with signed 4.12 scale");
    gte_command(m, 0x2e); CHECK(D[24] == 1024000 && D[7] == 250, "AVSZ4 averages all four depths");
    cw(30, -1); gte_command(m, 0x2e); CHECK(D[7] == 0 && C[31] == 0x80040000, "negative average saturates OTZ and sets SZ/OTZ flag");

    fresh(); identity(0); vec(0, 3, -4, 5); cw(5, 10); cw(6, 20); cw(7, 30); gte_command(m, SF | 0x12);
    CHECK(result(13, 16, 35) && D[9] == 13 && !C[31], "MVMVA adds translation before fraction shift");
    identity(8); vec(1, 6, 7, 8); gte_command(m, SF | (1u << 17) | (1u << 15) | (3u << 13) | 0x12);
    CHECK(result(6, 7, 8), "MVMVA selects light matrix, V1 and no translation");
    irs(9, 10, 11); identity(16); cw(13, 1); cw(14, 2); cw(15, 3); gte_command(m, SF | (2u << 17) | (3u << 15) | (1u << 13) | 0x12);
    CHECK(result(10, 12, 14), "MVMVA selects color matrix, IR vector and background");
    fresh(); identity(0); vec(0, 3, 4, 5); cw(21, 999); cw(22, 999); cw(23, 999); gte_command(m, SF | (2u << 13) | 0x12);
    CHECK(result(0, 4, 5), "MVMVA far-color bug discards translation and first product");
    fresh(); dw(6, 2); dw(8, 3); cw(1, 4); cw(2, 5); vec(0, 1, 2, 3); gte_command(m, (3u << 17) | (3u << 13) | 0x12);
    CHECK(result(41, 24, 30), "MVMVA reserved matrix uses red, IR0, RT13 and RT22");
    fresh(); irs(-4096, 2048, -1024); gte_command(m, SF | 0x28); CHECK(result(4096, 1024, 256), "SQR squares signed vector components with fraction shift");
    irs(32767, -32768, 1); gte_command(m, 0x28); CHECK(D[9] == 32767 && D[10] == 32767 && D[11] == 1 && (C[31] & 0x01800000) == 0x01800000, "SQR retains full MACs and flags IR1/2 saturation");
    fresh(); cw(0, 1); cw(2, 2); cw(4, 3); irs(4, 5, 6); gte_command(m, 0x0c); CHECK(result(-3, 6, -3), "OP is the cross product of RT diagonal and IR");
    fresh(); identity(0); vec(0, 32767, 0, 0); cw(5, INT32_MAX); gte_command(m, SF | 0x12);
    CHECK((C[31] & (1u << 30)) && (C[31] & 0x80000000), "matrix partial sums detect positive signed 44-bit overflow");
    fresh(); identity(0); vec(0, -32768, 0, 0); cw(5, 0x80000000); gte_command(m, SF | 0x12);
    CHECK((C[31] & (1u << 27)) && (C[31] & 0x80000000), "matrix partial sums detect negative signed 44-bit overflow");

    lighting(); gte_command(m, SF | LM | 0x1e); CHECK(result(1024, 2048, 3072) && D[22] == 0x20c08040, "NCS identity light/color matrices convert IR/16 to RGB");
    lighting(); vec(1, 2048, 1024, 512); vec(2, 512, 1024, 2048); gte_command(m, SF | LM | 0x20);
    CHECK(D[20] == 0x20c08040 && D[21] == 0x20204080 && D[22] == 0x20804020, "NCT lights three normals and preserves color FIFO order");
    lighting(); gte_command(m, SF | LM | 0x1b); CHECK(result(512, 512, 384) && D[22] == 0x20182020, "NCCS modulates light by primary color");
    lighting(); gte_command(m, SF | LM | 0x3f); CHECK(D[20] == 0x20182020 && D[21] == D[20] && D[22] == D[20], "NCCT modulates all three normals");
    lighting(); gte_command(m, SF | LM | 0x13); CHECK(result(1280, 768, 448) && D[22] == 0x201c3050, "NCDS interpolates modulated light halfway toward far color");
    lighting(); gte_command(m, SF | LM | 0x16); CHECK(D[20] == 0x201c3050 && D[21] == D[20] && D[22] == D[20], "NCDT depth-cues all three normals");
    lighting(); irs(1024, 2048, 3072); gte_command(m, SF | LM | 0x1c); CHECK(result(512, 512, 384) && D[22] == 0x20182020, "CC applies color matrix and primary modulation to IR vector");
    lighting(); irs(1024, 2048, 3072); gte_command(m, SF | LM | 0x14); CHECK(result(1280, 768, 448) && D[22] == 0x201c3050, "CDP adds depth cueing after color matrix/modulation");
    lighting(); irs(1024, 2048, 3072); gte_command(m, SF | LM | 0x29); CHECK(result(1280, 768, 448) && D[22] == 0x201c3050, "DCPL modulates and depth-cues the input IR vector");
    fresh(); dw(6, 0x20402010); dw(8, 2048); cw(21, 1024); cw(22, 2048); cw(23, 3072); gte_command(m, SF | 0x10);
    CHECK(result(640, 1280, 2048) && D[22] == 0x20805028, "DPCS halfway between primary RGB and far color");
    fresh(); dw(6, 0x20000000); dw(8, 0); dw(20, 0x00112233); dw(21, 0x00445566); dw(22, 0x00778899); gte_command(m, SF | 0x2a);
    CHECK(D[20] == 0x20112233 && D[21] == 0x20445566 && D[22] == 0x20778899, "DPCT reads successive RGB0 colors and takes code from RGBC");
    fresh(); irs(1024, 2048, 3072); dw(8, 2048); dw(6, 0x20000000); cw(21, 2048); cw(22, 1024); gte_command(m, SF | 0x11);
    CHECK(result(1536, 1536, 1536) && D[22] == 0x20606060, "INTPL halfway between input vector and far color");
    fresh(); irs(1024, -2048, 4096); dw(8, 2048); dw(6, 0x20000000); gte_command(m, SF | 0x3d);
    CHECK(result(512, -1024, 2048) && D[22] == 0x20800020 && C[31] == (1u << 20), "GPF scales IR, preserves negative IR and flags RGB clamp independently");
    fresh(); irs(2, 4, 6); dw(8, 4096); dw(25, 100); dw(26, 200); dw(27, 300); gte_command(m, SF | 0x3e);
    CHECK(result(102, 204, 306) && (D[22] & 0xffffff) == 0x130c06, "GPL adds previous MAC base with sf rescaling");

    fresh(); m->cpu.r[1] = 0x12348900; RUN(M2(4, 1, 1), M2(0, 2, 1), I(9, 2, 3, 0), I(9, 2, 4, 0));
    CHECK(m->cpu.r[2] == 0xffff8900 && m->cpu.r[3] == 0 && m->cpu.r[4] == 0xffff8900, "MTC2/MFC2 use transfer side effects and CPU load delay");
    fresh(); m->cpu.r[1] = 0x1234ffff; RUN(M2(6, 1, 26), M2(2, 2, 26), 0);
    CHECK(m->cpu.r[2] == 0xffffffff, "CTC2/CFC2 preserve H's signed transfer behavior");
    fresh(); m->cpu.r[1] = 0x80002000; mem_write32(m, 0x80002000, 0x11223344); dw(12, 1); dw(13, 2); dw(14, 3);
    RUN(I(0x32, 1, 15, 0), I(0x3a, 1, 15, 4));
    CHECK(D[12] == 2 && D[13] == 3 && mem_read32(m, 0x80002004) == 0x11223344, "LWC2/SWC2 use SXYP push and mirror semantics");
    fresh(); irs(2, 3, 4); RUN(0x4a000028, M2(0, 2, 25), 0);   // cached: a cycle each, and MFC2 waits out SQR's 5
    CHECK(m->cpu.r[2] == 4 && D[26] == 9 && D[27] == 16 && m->cycles == 7, "COP2 command dispatch and dependent MFC2 interlock execute through interpreter");
    ps1 *other = ps1_new(); CHECK(other && !other->cpu.cop2d[25] && !other->cpu.gte_busy && D[25] == 4, "GTE registers and deadlines are independent per machine"); ps1_free(other);
    printf("gte: %d checks, %d failed\n", checks, fails); ps1_free(m); return fails != 0;
}
