// COP2's fixed-point geometry and lighting engine (plan.md, phase 2), derived from psx-spx, Geometry Transformation Engine: GTE Registers, GTE Saturation, Coordinate
// Calculation Commands, General Purpose Calculation Commands, Color Calculation Commands, and Division Inaccuracy. No host floating point: matrices are signed 4.12,
// multiply/accumulate intermediates wrap at 44 bits, output MAC registers at 32 bits, and IR/FIFO clamps set their documented independent flags. t_gte supplies synthetic
// numeric cases, including depth projection and the hardware's non-exact reciprocal. All persistent registers and the busy deadline are in ps1.cpu
// https://psx-spx.consoledev.net/geometrytransformationenginegte/
#include "ps1.h"
#include "gte.h"
#include <limits.h>

#define D (m->cpu.cop2d)
#define C (m->cpu.cop2c)

static int32_t clamp(int64_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : (int32_t)v; }
static void summary(ps1 *m) { C[31] = (C[31] & 0x7ffff000) | ((C[31] & 0x7f87e000) ? 0x80000000 : 0); }
static void wait_gte(ps1 *m) { if (m->cycles < m->cpu.gte_busy) m->cycles = m->cpu.gte_busy; }

uint32_t gte_read(ps1 *m, unsigned reg, int control) {
    wait_gte(m); reg &= 31;
    if (control) return C[reg];
    if (reg == 15) return D[14];
    if (reg == 28 || reg == 29) {
        uint32_t out = 0;
        for (int i = 0; i < 3; i++) out |= (uint32_t)clamp((int16_t)D[9 + i] >> 7, 0, 31) << (5 * i);
        return out;
    }
    return D[reg];
}
void gte_write(ps1 *m, unsigned reg, uint32_t v, int control) {
    reg &= 31;
    // GTE Overview documents stalls for reads/commands. Results here are materialized at issue, so a write racing an in-flight output has no staged pipeline model yet
    if (m->cycles < m->cpu.gte_busy) RED_ONCE(m, RED_GTE_TIMING, "gte: write during an executing command has no transfer pipeline timing yet");
    if (control) {
        switch (reg) { case 4: case 12: case 20: case 26: case 27: case 29: case 30: v = (uint32_t)(int32_t)(int16_t)v; break; }
        C[reg] = v; if (reg == 31) summary(m);
        return;
    }
    switch (reg) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: v = (uint32_t)(int32_t)(int16_t)v; break;
    case 7: case 16: case 17: case 18: case 19: v &= 0xffff; break;
    case 15: D[12] = D[13]; D[13] = D[14]; D[14] = v; return;
    case 28: for (int i = 0; i < 3; i++) D[9 + i] = (v >> (5 * i) & 31) * 128; return;
    case 29: case 31: return;
    case 30: { uint32_t bits = (int32_t)v < 0 ? ~v : v; D[31] = bits ? (uint32_t)__builtin_clz(bits) : 32; break; }
    }
    D[reg] = v;
}

// GTE Saturation: each partial sum is checked before wrapping to signed 44 bits. A later cancellation cannot undo an overflow flag
static int64_t accum(ps1 *m, int i, int64_t v) {
    if (v > INT64_C(0x7ffffffffff)) C[31] |= 1u << (31 - i);
    if (v < -INT64_C(0x80000000000)) C[31] |= 1u << (28 - i);
    uint64_t low = (uint64_t)v & UINT64_C(0xfffffffffff);
    return (int64_t)(low & (UINT64_C(1) << 43) ? low | UINT64_C(0xfffff00000000000) : low);
}
static int32_t limited(ps1 *m, int64_t v, int32_t lo, int32_t hi, int flag) {
    if (v < lo || v > hi) C[31] |= 1u << flag;
    return clamp(v, lo, hi);
}
static void ir(ps1 *m, int i, int64_t v, int lm) { D[8 + i] = (uint32_t)limited(m, v, lm ? 0 : -32768, 32767, 25 - i); }
static void mac(ps1 *m, const int64_t raw[3], int sf, int lm) {
    for (int i = 0; i < 3; i++) { D[25 + i] = (uint32_t)(accum(m, i + 1, raw[i]) >> sf); ir(m, i + 1, (int32_t)D[25 + i], lm); }
}
static void mac0(ps1 *m, int64_t v) {
    if (v > INT32_MAX) C[31] |= 1u << 16;
    if (v < INT32_MIN) C[31] |= 1u << 15;
    D[24] = (uint32_t)v;
}
static void vector(ps1 *m, int n, int32_t out[3]) {
    if (n == 3) { for (int i = 0; i < 3; i++) out[i] = (int16_t)D[9 + i]; }
    else { out[0] = (int16_t)D[n * 2]; out[1] = (int16_t)(D[n * 2] >> 16); out[2] = (int16_t)D[n * 2 + 1]; }
}
static void matrix(ps1 *m, int n, int32_t out[9]) {
    if (n < 3) for (int i = 0; i < 9; i++) out[i] = (int16_t)(C[n * 8 + i / 2] >> (16 * (i & 1)));
    else {   // MVMVA reserved matrix selection is observable hardware behavior, not a fourth stored matrix
        out[0] = -(int32_t)(D[6] & 255) * 16; out[1] = -out[0]; out[2] = (int16_t)D[8];
        for (int i = 3; i < 6; i++) out[i] = (int16_t)C[1];
        for (int i = 6; i < 9; i++) out[i] = (int16_t)C[2];
    }
}
static void multiply(ps1 *m, int mx, int vx, int cv, int sf, int lm, int64_t raw[3]) {
    int32_t mat[9], v[3]; matrix(m, mx, mat); vector(m, vx, v);
    for (int i = 0; i < 3; i++) {
        int64_t t = cv == 3 ? 0 : (int64_t)(int32_t)C[5 + cv * 8 + i] * 4096;
        t = accum(m, i + 1, t + (int64_t)mat[i * 3] * v[0]);
        if (cv == 2) {   // General Purpose Commands, MVMVA: FC plus the first matrix product is discarded, but its saturation flags remain
            ir(m, i + 1, (int32_t)(t >> sf), 0); t = 0;
        }
        t = accum(m, i + 1, t + (int64_t)mat[i * 3 + 1] * v[1]);
        raw[i] = accum(m, i + 1, t + (int64_t)mat[i * 3 + 2] * v[2]);
    }
    mac(m, raw, sf, lm);
}
static void rgb(ps1 *m) {
    uint32_t color = D[6] & 0xff000000;
    for (int i = 0; i < 3; i++) color |= (uint32_t)limited(m, (int32_t)D[25 + i] >> 4, 0, 255, 21 - i) << (8 * i);
    D[20] = D[21]; D[21] = D[22]; D[22] = color;
}
static void interpolate(ps1 *m, int64_t raw[3], int sf, int lm) {
    for (int i = 0; i < 3; i++) {
        int64_t difference = accum(m, i + 1, (int64_t)(int32_t)C[21 + i] * 4096 - raw[i]);
        ir(m, i + 1, (int32_t)(difference >> sf), 0);
        raw[i] = accum(m, i + 1, raw[i] + (int64_t)(int16_t)D[9 + i] * (int16_t)D[8]);
    }
    mac(m, raw, sf, lm);
}
static void modulation(ps1 *m, int64_t raw[3]) { for (int i = 0; i < 3; i++) raw[i] = (int64_t)(D[6] >> (8 * i) & 255) * (int16_t)D[9 + i] * 16; }

// Division Inaccuracy: generate the documented UNR table entry directly; the 64-bit product preserves its rounding, including an unflagged final clamp to 1FFFFh
static uint32_t divide(ps1 *m, uint32_t h, uint32_t z) {
    if (h >= z * 2) { C[31] |= 1u << 17; return 0x1ffff; }
    unsigned shift = (unsigned)__builtin_clz(z) - 16;
    uint32_t n = h << shift, d = z << shift, index = (d - 0x7fc0) >> 7;
    int32_t table = (int32_t)((0x40000 / (index + 0x100) + 1) / 2) - 0x101;
    uint32_t u = (uint32_t)(table < 0 ? 0 : table) + 0x101;
    d = (0x2000080 - d * u) >> 8; d = (0x80 + d * u) >> 8;
    uint32_t q = (uint32_t)(((uint64_t)n * d + 0x8000) >> 16);
    return q > 0x1ffff ? 0x1ffff : q;
}
static void project(ps1 *m, int vx, int sf, int lm, int depth) {
    uint32_t prior_ir3 = C[31] & (1u << 22); int64_t raw[3]; multiply(m, 0, vx, 0, sf, lm, raw);
    // Coordinate Commands: the IR3 flag tests raw Z >>12 with lm=0, independently of the sf/lm used to store IR3
    C[31] = (C[31] & ~(1u << 22)) | prior_ir3;
    limited(m, raw[2] >> 12, -32768, 32767, 22);
    D[16] = D[17]; D[17] = D[18]; D[18] = D[19]; D[19] = (uint32_t)limited(m, raw[2] >> 12, 0, 65535, 18);
    uint32_t q = divide(m, (uint16_t)C[26], D[19]), xy = 0;
    for (int i = 0; i < 2; i++) {
        int64_t p = (int64_t)(int16_t)D[9 + i] * q + (int32_t)C[24 + i]; mac0(m, p);
        xy |= (uint32_t)(uint16_t)limited(m, p >> 16, -1024, 1023, 14 - i) << (16 * i);
    }
    D[12] = D[13]; D[13] = D[14]; D[14] = xy;
    if (depth) { int64_t p = (int64_t)(int16_t)C[27] * q + (int32_t)C[28]; mac0(m, p); D[8] = (uint32_t)limited(m, p >> 12, 0, 4096, 12); }
}

void gte_command(ps1 *m, uint32_t op) {
    wait_gte(m); C[31] = 0;
    int sf = op & (1u << 19) ? 12 : 0, lm = !!(op & (1u << 10)), cycles = 1; int64_t raw[3] = {0};
    switch (op & 63) {
    case 0x01: case 0x30: {
        int n = (op & 63) == 1 ? 1 : 3; cycles = n == 1 ? 15 : 23;
        for (int v = 0; v < n; v++) { uint32_t flags = C[31] & (1u << 22); project(m, v, sf, lm, v == n - 1); C[31] |= flags; }
        break;
    }
    case 0x06: {
        int64_t x[3], y[3]; for (int i = 0; i < 3; i++) { x[i] = (int16_t)D[12 + i]; y[i] = (int16_t)(D[12 + i] >> 16); }
        mac0(m, x[0] * (y[1] - y[2]) + x[1] * (y[2] - y[0]) + x[2] * (y[0] - y[1])); cycles = 8; break;
    }
    case 0x0c: {
        int32_t v[3], a[3] = {(int16_t)C[0], (int16_t)C[2], (int16_t)C[4]}; vector(m, 3, v);
        for (int i = 0; i < 3; i++) raw[i] = (int64_t)v[(i + 2) % 3] * a[(i + 1) % 3] - (int64_t)v[(i + 1) % 3] * a[(i + 2) % 3];
        mac(m, raw, sf, lm); cycles = 6; break;
    }
    case 0x12: multiply(m, op >> 17 & 3, op >> 15 & 3, op >> 13 & 3, sf, lm, raw); cycles = 8; break;
    case 0x28: for (int i = 0; i < 3; i++) raw[i] = (int64_t)(int16_t)D[9 + i] * (int16_t)D[9 + i]; mac(m, raw, sf, lm); cycles = 5; break;
    case 0x2d: case 0x2e: {
        int four = (op & 63) == 0x2e; int64_t sum = 0;
        for (int i = four ? 16 : 17; i <= 19; i++) sum += (uint16_t)D[i];
        int64_t p = sum * (int16_t)C[four ? 30 : 29]; mac0(m, p); D[7] = (uint32_t)limited(m, p >> 12, 0, 65535, 18); cycles = four ? 6 : 5; break;
    }
    case 0x3d: case 0x3e:
        for (int i = 0; i < 3; i++) raw[i] = (int64_t)(int16_t)D[9 + i] * (int16_t)D[8] + ((op & 63) == 0x3e ? (int64_t)(int32_t)D[25 + i] * (sf ? 4096 : 1) : 0);
        mac(m, raw, sf, lm); rgb(m); cycles = 5; break;
    case 0x10: case 0x11: case 0x29: case 0x2a: {
        unsigned cmd = op & 63; int n = cmd == 0x2a ? 3 : 1; cycles = n == 3 ? 17 : 8;
        for (int v = 0; v < n; v++) {
            if (cmd == 0x29) modulation(m, raw);
            else for (int i = 0; i < 3; i++) raw[i] = cmd == 0x11 ? (int64_t)(int16_t)D[9 + i] * 4096 : (int64_t)((cmd == 0x2a ? D[20] : D[6]) >> (8 * i) & 255) * 65536;
            interpolate(m, raw, sf, lm); rgb(m);
        }
        break;
    }
    case 0x13: case 0x16: case 0x1b: case 0x1c: case 0x1e: case 0x20: case 0x3f: case 0x14: {
        unsigned cmd = op & 63; int n = cmd == 0x16 || cmd == 0x20 || cmd == 0x3f ? 3 : 1;
        cycles = cmd == 0x13 ? 19 : cmd == 0x16 ? 44 : cmd == 0x1b ? 17 : cmd == 0x1c ? 11 : cmd == 0x1e ? 14 : cmd == 0x20 ? 30 : cmd == 0x3f ? 39 : 13;
        for (int v = 0; v < n; v++) {
            if (cmd != 0x1c && cmd != 0x14) multiply(m, 1, v, 3, sf, lm, raw);
            multiply(m, 2, 3, 1, sf, lm, raw);
            if (cmd != 0x1e && cmd != 0x20) {
                modulation(m, raw);
                if (cmd == 0x13 || cmd == 0x16 || cmd == 0x14) interpolate(m, raw, sf, lm); else mac(m, raw, sf, lm);
            }
            rgb(m);
        }
        break;
    }
    default: RED_ONCE(m, RED_GTE, "gte: undocumented command %02x is not modelled", op & 63); break;
    }
    summary(m); m->cpu.gte_busy = m->cycles + (uint64_t)cycles;
}
