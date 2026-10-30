// The 24-voice SPU mixer (plan.md, phase 2), written from public documentation: https://psx-spx.consoledev.net/soundprocessingunitspu/ (ADPCM Samples, ADPCM Pitch,
// Volume and ADSR Generator, Voice Flags, Noise Generator, Memory Access, Interrupt). Predictor constants: https://psx-spx.consoledev.net/cdromformat/#cdrom-xa-audio-adpcm-compression.
// Evidence: t_spu keys a synthetic block on; the former register-only SPU always returned zero ENVX and produced no samples. Its synthetic reflection also showed that
// EON/ATTR previously left reverb RAM unchanged. Voices now advance at 44100 Hz, reverb at 22050 Hz. External CD/PIO mixing and sub-sample RAM/FIFO timing remain
// explicitly reported gaps. No emulator source was consulted
#include "ps1.h"
#include <stddef.h>
#include <string.h>

#define S (&m->spu)
#define REG(off) S->reg[(off) / 2]
enum { R_KON = 0x188, R_KOFF = 0x18c, R_END = 0x19c, R_IRQ = 0x1a4, R_ADDR = 0x1a6, R_DATA = 0x1a8, R_CNT = 0x1aa, R_STAT = 0x1ae };
enum { OFF, ATTACK, DECAY, SUSTAIN, RELEASE };

// Fixed hardware coefficients from psx-spx, SPU ADPCM Pitch / 4-Point Gaussian Interpolation; hardware data, not emulator source
static const int16_t gauss[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1,
    0x1, 0x1, 0x1, 0x2, 0x2, 0x2, 0x3, 0x3,
    0x3, 0x4, 0x4, 0x5, 0x5, 0x6, 0x7, 0x7,
    0x8, 0x9, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe,
    0xf, 0x10, 0x11, 0x12, 0x13, 0x15, 0x16, 0x18,
    0x19, 0x1b, 0x1c, 0x1e, 0x20, 0x21, 0x23, 0x25,
    0x27, 0x29, 0x2c, 0x2e, 0x30, 0x33, 0x35, 0x38,
    0x3a, 0x3d, 0x40, 0x43, 0x46, 0x49, 0x4d, 0x50,
    0x54, 0x57, 0x5b, 0x5f, 0x63, 0x67, 0x6b, 0x6f,
    0x74, 0x78, 0x7d, 0x82, 0x87, 0x8c, 0x91, 0x96,
    0x9c, 0xa1, 0xa7, 0xad, 0xb3, 0xba, 0xc0, 0xc7,
    0xcd, 0xd4, 0xdb, 0xe3, 0xea, 0xf2, 0xfa, 0x101,
    0x10a, 0x112, 0x11b, 0x123, 0x12c, 0x135, 0x13f, 0x148,
    0x152, 0x15c, 0x166, 0x171, 0x17b, 0x186, 0x191, 0x19c,
    0x1a8, 0x1b4, 0x1c0, 0x1cc, 0x1d9, 0x1e5, 0x1f2, 0x200,
    0x20d, 0x21b, 0x229, 0x237, 0x246, 0x255, 0x264, 0x273,
    0x283, 0x293, 0x2a3, 0x2b4, 0x2c4, 0x2d6, 0x2e7, 0x2f9,
    0x30b, 0x31d, 0x330, 0x343, 0x356, 0x36a, 0x37e, 0x392,
    0x3a7, 0x3bc, 0x3d1, 0x3e7, 0x3fc, 0x413, 0x42a, 0x441,
    0x458, 0x470, 0x488, 0x4a0, 0x4b9, 0x4d2, 0x4ec, 0x506,
    0x520, 0x53b, 0x556, 0x572, 0x58e, 0x5aa, 0x5c7, 0x5e4,
    0x601, 0x61f, 0x63e, 0x65c, 0x67c, 0x69b, 0x6bb, 0x6dc,
    0x6fd, 0x71e, 0x740, 0x762, 0x784, 0x7a7, 0x7cb, 0x7ef,
    0x813, 0x838, 0x85d, 0x883, 0x8a9, 0x8d0, 0x8f7, 0x91e,
    0x946, 0x96f, 0x998, 0x9c1, 0x9eb, 0xa16, 0xa40, 0xa6c,
    0xa98, 0xac4, 0xaf1, 0xb1e, 0xb4c, 0xb7a, 0xba9, 0xbd8,
    0xc07, 0xc38, 0xc68, 0xc99, 0xccb, 0xcfd, 0xd30, 0xd63,
    0xd97, 0xdcb, 0xe00, 0xe35, 0xe6b, 0xea1, 0xed7, 0xf0f,
    0xf46, 0xf7f, 0xfb7, 0xff1, 0x102a, 0x1065, 0x109f, 0x10db,
    0x1116, 0x1153, 0x118f, 0x11cd, 0x120b, 0x1249, 0x1288, 0x12c7,
    0x1307, 0x1347, 0x1388, 0x13c9, 0x140b, 0x144d, 0x1490, 0x14d4,
    0x1517, 0x155c, 0x15a0, 0x15e6, 0x162c, 0x1672, 0x16b9, 0x1700,
    0x1747, 0x1790, 0x17d8, 0x1821, 0x186b, 0x18b5, 0x1900, 0x194b,
    0x1996, 0x19e2, 0x1a2e, 0x1a7b, 0x1ac8, 0x1b16, 0x1b64, 0x1bb3,
    0x1c02, 0x1c51, 0x1ca1, 0x1cf1, 0x1d42, 0x1d93, 0x1de5, 0x1e37,
    0x1e89, 0x1edc, 0x1f2f, 0x1f82, 0x1fd6, 0x202a, 0x207f, 0x20d4,
    0x2129, 0x217f, 0x21d5, 0x222c, 0x2282, 0x22da, 0x2331, 0x2389,
    0x23e1, 0x2439, 0x2492, 0x24eb, 0x2545, 0x259e, 0x25f8, 0x2653,
    0x26ad, 0x2708, 0x2763, 0x27be, 0x281a, 0x2876, 0x28d2, 0x292e,
    0x298b, 0x29e7, 0x2a44, 0x2aa1, 0x2aff, 0x2b5c, 0x2bba, 0x2c18,
    0x2c76, 0x2cd4, 0x2d33, 0x2d91, 0x2df0, 0x2e4f, 0x2eae, 0x2f0d,
    0x2f6c, 0x2fcc, 0x302b, 0x308b, 0x30ea, 0x314a, 0x31aa, 0x3209,
    0x3269, 0x32c9, 0x3329, 0x3389, 0x33e9, 0x3449, 0x34a9, 0x3509,
    0x3569, 0x35c9, 0x3629, 0x3689, 0x36e8, 0x3748, 0x37a8, 0x3807,
    0x3867, 0x38c6, 0x3926, 0x3985, 0x39e4, 0x3a43, 0x3aa2, 0x3b00,
    0x3b5f, 0x3bbd, 0x3c1b, 0x3c79, 0x3cd7, 0x3d35, 0x3d92, 0x3def,
    0x3e4c, 0x3ea9, 0x3f05, 0x3f62, 0x3fbd, 0x4019, 0x4074, 0x40d0,
    0x412a, 0x4185, 0x41df, 0x4239, 0x4292, 0x42eb, 0x4344, 0x439c,
    0x43f4, 0x444c, 0x44a3, 0x44fa, 0x4550, 0x45a6, 0x45fc, 0x4651,
    0x46a6, 0x46fa, 0x474e, 0x47a1, 0x47f4, 0x4846, 0x4898, 0x48e9,
    0x493a, 0x498a, 0x49d9, 0x4a29, 0x4a77, 0x4ac5, 0x4b13, 0x4b5f,
    0x4bac, 0x4bf7, 0x4c42, 0x4c8d, 0x4cd7, 0x4d20, 0x4d68, 0x4db0,
    0x4df7, 0x4e3e, 0x4e84, 0x4ec9, 0x4f0e, 0x4f52, 0x4f95, 0x4fd7,
    0x5019, 0x505a, 0x509a, 0x50da, 0x5118, 0x5156, 0x5194, 0x51d0,
    0x520c, 0x5247, 0x5281, 0x52ba, 0x52f3, 0x532a, 0x5361, 0x5397,
    0x53cc, 0x5401, 0x5434, 0x5467, 0x5499, 0x54ca, 0x54fa, 0x5529,
    0x5558, 0x5585, 0x55b2, 0x55de, 0x5609, 0x5632, 0x565b, 0x5684,
    0x56ab, 0x56d1, 0x56f6, 0x571b, 0x573e, 0x5761, 0x5782, 0x57a3,
    0x57c3, 0x57e2, 0x57ff, 0x581c, 0x5838, 0x5853, 0x586d, 0x5886,
    0x589e, 0x58b5, 0x58cb, 0x58e0, 0x58f4, 0x5907, 0x5919, 0x592a,
    0x593a, 0x5949, 0x5958, 0x5965, 0x5971, 0x597c, 0x5986, 0x598f,
    0x5997, 0x599e, 0x59a4, 0x59a9, 0x59ad, 0x59b0, 0x59b2, 0x59b3,
};
static int32_t clamp(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static uint32_t bits(ps1 *m, unsigned off) { return REG(off) | (uint32_t)(REG(off + 2) & 0xff) << 16; }
static void ram_irq(ps1 *m, uint32_t addr) {
    if ((REG(R_CNT) & 0x8040) == 0x8040 && (addr & 0x7fff8) == (uint32_t)REG(R_IRQ) * 8) {
        if (!S->irq) irq_raise(m, IRQ_SPU);
        S->irq = 1;
    }
}
static void ram_put(ps1 *m, uint32_t addr, uint16_t v) {
    addr &= SPU_RAM_SIZE - 1; ram_irq(m, addr); S->ram[addr] = (uint8_t)v; S->ram[(addr + 1) & (SPU_RAM_SIZE - 1)] = (uint8_t)(v >> 8);
}
static uint16_t ram_get(ps1 *m, uint32_t addr) {
    addr &= SPU_RAM_SIZE - 1; ram_irq(m, addr); return (uint16_t)(S->ram[addr] | (uint16_t)S->ram[(addr + 1) & (SPU_RAM_SIZE - 1)] << 8);
}
static void put(ps1 *m, uint16_t v) { ram_put(m, S->addr, v); S->addr = (S->addr + 2) & (SPU_RAM_SIZE - 1); }
static uint16_t get(ps1 *m) { uint16_t v = ram_get(m, S->addr); S->addr = (S->addr + 2) & (SPU_RAM_SIZE - 1); return v; }

// Volume/ADSR: a 15-bit fractional rate counter, not a rounded integer period. All-ones rates stop; rates above shift26 still accumulate one counter unit
static int32_t envelope(int32_t level, uint32_t *counter, int shift, int stepval, int decreasing, int exponential, int negative, int all) {
    int32_t step = 7 - stepval;
    if (decreasing ^ negative) step = ~step;
    step *= 1 << (shift < 11 ? 11 - shift : 0);
    uint32_t inc = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    if (exponential && !decreasing && level > 0x6000) {
        if (shift < 10) step >>= 2;
        else if (shift >= 11) inc >>= 2;
        else { step >>= 1; inc >>= 1; }
    } else if (exponential && decreasing) step = (int32_t)(((int64_t)step * level) >> 15);
    if (!all && !inc) inc = 1;
    *counter += inc;
    if (!(*counter & 0x8000)) return level;
    *counter = 0; level += step;
    return !decreasing ? clamp(level, -32768, 32767) : negative ? clamp(level, -32768, 0) : (level < 0 ? 0 : level);
}
static void volume(ps1_spu_volume *v, uint16_t control) {
    if (!(control & 0x8000)) { v->level = (int16_t)(control * 2); v->counter = 0; return; }
    v->level = envelope(v->level, &v->counter, control >> 2 & 31, control & 3, control >> 13 & 1, control >> 14 & 1, control >> 12 & 1, (control & 0x7f) == 0x7f);
}
static void adsr(ps1 *m, int n) {
    ps1_spu_voice *v = &S->voice[n]; uint16_t a = REG(n * 16 + 8), b = REG(n * 16 + 10);
    int shift = 0, step = 0, dec = 0, exp = 0, all = 0;
    switch (v->phase) {
    case ATTACK: shift = a >> 10 & 31; step = a >> 8 & 3; exp = a >> 15; all = (a & 0x7f00) == 0x7f00; break;
    case DECAY: shift = a >> 4 & 15; dec = exp = 1; break;
    case SUSTAIN: shift = b >> 8 & 31; step = b >> 6 & 3; dec = b >> 14 & 1; exp = b >> 15; all = (b & 0x1fc0) == 0x1fc0; break;
    case RELEASE: shift = b & 31; dec = 1; exp = b >> 5 & 1; all = shift == 31; break;
    default: v->env = 0; REG(n * 16 + 12) = 0; return;
    }
    v->env = envelope(v->env, &v->env_counter, shift, step, dec, exp, 0, all);
    int next = v->phase;
    if (v->phase == ATTACK && v->env >= 32767) next = DECAY;
    else if (v->phase == DECAY && v->env <= ((a & 15) + 1) * 0x800) next = SUSTAIN;
    else if (v->phase == RELEASE && !v->env) next = OFF;
    if (v->phase != next) { v->phase = next; v->env_counter = 0; }
    REG(n * 16 + 12) = (uint16_t)v->env;
}
static void decode(ps1 *m, int n) {
    // Blocks are latched whole for this first mixer. Mid-block RAM mutations and exact per-word fetch times still need the sub-sample model reported at KON
    ps1_spu_voice *v = &S->voice[n]; uint8_t raw[16];
    for (int i = 0; i < 16; i += 2) { uint16_t w = ram_get(m, v->addr + (unsigned)i); raw[i] = (uint8_t)w; raw[i + 1] = (uint8_t)(w >> 8); }
    memmove(v->decoded, v->decoded + 28, 3 * sizeof *v->decoded);
    int shift = raw[0] & 15, filter = raw[0] >> 4 & 7;
    if (shift > 12) shift = 9;   // SPU-ADPCM vs XA-ADPCM: reserved shifts13..15 use9
    if (filter > 4) { RED_ONCE(m, RED_SPU_EFFECT, "spu: reserved ADPCM predictor filter is not modelled"); filter = 0; }
    static const int pos[5] = {0, 60, 115, 98, 122}, neg[5] = {0, 0, -52, -55, -60};
    for (int i = 0; i < 28; i++) {
        int nibble = raw[2 + i / 2] >> ((i & 1) * 4) & 15; if (nibble & 8) nibble -= 16;
        int32_t sample = ((nibble * 4096) >> shift) + ((v->old * pos[filter] + v->older * neg[filter] + 32) >> 6);
        sample = clamp(sample, -32768, 32767); v->decoded[i + 3] = (int16_t)sample; v->older = v->old; v->old = (int16_t)sample;
    }
    v->flags = raw[1]; v->loaded = 1;
    if (v->flags & 4) REG(n * 16 + 14) = (uint16_t)(v->addr / 8);
}
static int32_t voice_sample(ps1 *m, int n, int32_t previous) {
    ps1_spu_voice *v = &S->voice[n];
    ram_irq(m, v->addr);   // Voice Interrupt: silent, released and noise voices still fetch their ADPCM header
    if (!v->loaded) decode(m, n);
    int i = (int)(v->pitch >> 12), f = v->pitch >> 4 & 255; const int16_t *p = v->decoded + i;
    int32_t sample = (gauss[255 - f] * p[0] >> 15) + (gauss[511 - f] * p[1] >> 15) + (gauss[256 + f] * p[2] >> 15) + (gauss[f] * p[3] >> 15);
    if (bits(m, 0x194) >> n & 1) sample = (int16_t)S->noise;
    sample = (sample * v->env) >> 15;
    unsigned pitch = REG(n * 16 + 4);
    if (n && (bits(m, 0x190) >> n & 1)) pitch = (unsigned)(((int64_t)(int16_t)pitch * (previous + 0x8000)) >> 15) & 0xffff;
    if (pitch > 0x3fff) pitch = 0x4000;
    v->pitch += pitch;
    if (v->pitch >= 28 * 0x1000u) {
        v->pitch -= 28 * 0x1000u; v->loaded = 0;
        if (v->flags & 1) {
            S->endx |= 1u << n; v->addr = (uint32_t)REG(n * 16 + 14) * 8;
            if (!(v->flags & 2)) { v->env = 0; v->phase = RELEASE; v->env_counter = 0; }
        } else v->addr = (v->addr + 16) & (SPU_RAM_SIZE - 1);
    }
    return sample;
}
static void key_on(ps1 *m, int n) {
    ps1_spu_voice *v = &S->voice[n]; ps1_spu_volume vol[2] = {v->volume[0], v->volume[1]};
    memset(v, 0, sizeof *v); v->volume[0] = vol[0]; v->volume[1] = vol[1];
    v->addr = (uint32_t)REG(n * 16 + 6) * 8; v->phase = ATTACK; REG(n * 16 + 12) = 0; S->endx &= ~(1u << n);
}
// Reverb Formula / Buffer Resampling: half-rate stereo reflections, four combs and two all-pass stages, with the documented39-tap hardware FIR on input/output.
// The memory clock alternates left/right work; exact within-sample ordering of voice/reverb accesses remains part of the reported RAM timing gap
static const int16_t reverb_fir[39] = {-1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384, 10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1};
static int32_t q15(int32_t x, int32_t volume) { return (int32_t)(((int64_t)x * volume) >> 15); }
static uint32_t reverb_addr(ps1 *m, int32_t offset) {
    int32_t base = REG(0x1a2) * 8, size = SPU_RAM_SIZE - base;
    int32_t at = ((int32_t)S->reverb_addr - base + offset) % size;
    if (at < 0) at += size;
    return (uint32_t)(base + at);
}
static int32_t reverb_get(ps1 *m, int32_t offset) { return (int16_t)ram_get(m, reverb_addr(m, offset)); }
static void reverb_put(ps1 *m, int32_t offset, int32_t value) {
    uint32_t addr = reverb_addr(m, offset);
    if (REG(R_CNT) & 0x80) ram_put(m, addr, (uint16_t)clamp(value, -32768, 32767));
    else (void)ram_get(m, addr);   // Reverb Disable / Computation Order: disabled writes become RAM reads; their IRQ address comparisons still run
}
static int32_t reverb_channel(ps1 *m, int ch, int32_t input) {
    int32_t viir = (int16_t)REG(0x1c4), wall = (int16_t)REG(0x1ce);
    int32_t same = REG(0x1d4 + ch * 2) * 8, diff = REG(0x1e4 + ch * 2) * 8;
    int32_t old = reverb_get(m, same - 2), source = reverb_get(m, REG(0x1e0 + ch * 2) * 8);
    int32_t value = q15(q15(input, (int16_t)REG(0x1fc + ch * 2)) + q15(source, wall) - old, viir) + old;
    // Reverb Formula / Bug: -8000h vIIR negates the complete reflection result, including its old-sample addition
    reverb_put(m, same, viir == -32768 ? -value : value);
    old = reverb_get(m, diff - 2); source = reverb_get(m, REG(0x1f0 + (ch ^ 1) * 2) * 8);
    value = q15(q15(input, (int16_t)REG(0x1fc + ch * 2)) + q15(source, wall) - old, viir) + old;
    reverb_put(m, diff, viir == -32768 ? -value : value);
    static const unsigned comb[4] = {0x1d8, 0x1dc, 0x1e8, 0x1ec};
    int32_t out = 0;
    for (int i = 0; i < 4; i++) out += q15(reverb_get(m, REG(comb[i] + ch * 2) * 8), (int16_t)REG(0x1c6 + i * 2));
    for (int i = 0; i < 2; i++) {
        int32_t dst = REG(0x1f4 + i * 4 + ch * 2) * 8, delay = REG(0x1c0 + i * 2) * 8;
        int32_t feedback = reverb_get(m, dst - delay), volume = (int16_t)REG(0x1d0 + i * 2);
        value = clamp(out - q15(feedback, volume), -32768, 32767); reverb_put(m, dst, value);
        out = clamp(q15(value, volume) + feedback, -32768, 32767);
    }
    return out;
}
static int32_t fir(const int16_t *history, unsigned at, int shift) {
    int64_t sum = 0;
    for (unsigned i = 0; i < 39; i++) sum += (int32_t)history[(at - i) & 63] * reverb_fir[i];
    return clamp((int32_t)(sum >> shift), -32768, 32767);
}
static void reverb(ps1 *m, const int32_t *input, int32_t *output) {
    unsigned at = S->reverb_sample & 63, ch = S->reverb_phase;
    for (int c = 0; c < 2; c++) { S->reverb_in[c][at] = (int16_t)clamp(input[c], -32768, 32767); S->reverb_up[c][at] = 0; }
    if (REG(0x1ac) & 12) {
        S->reverb_last[ch] = (int16_t)reverb_channel(m, (int)ch, fir(S->reverb_in[ch], at, 15));
        S->reverb_up[ch][at] = S->reverb_last[ch];
        if (ch) {
            S->reverb_addr += 2;
            if (S->reverb_addr >= SPU_RAM_SIZE) S->reverb_addr = (uint32_t)REG(0x1a2) * 8;
        }
    }
    // Zero insertion halves the interpolator's DC gain, so the upsampling FIR has twice the downsampling gain
    for (int c = 0; c < 2; c++) output[c] = q15(fir(S->reverb_up[c], at, 14), (int16_t)REG(0x184 + c * 2));
    S->reverb_phase ^= 1; S->reverb_sample++;
}
void spu_reset(ps1 *m) { memset(S, 0, offsetof(ps1_spu, ram)); S->next = m->cycles + CPU_HZ / 44100; }
void spu_event(ps1 *m) {
    S->next += CPU_HZ / 44100; uint16_t cnt = REG(R_CNT); REG(R_STAT) = cnt & 0x3f;
    if ((cnt >> 4 & 3) == 1) { for (int i = 0; i < S->nfifo; i++) put(m, S->fifo[i]); S->nfifo = 0; }
    S->noise_timer -= 4 + (cnt >> 8 & 3);
    if (S->noise_timer < 0) {
        unsigned parity = ((S->noise >> 15) ^ (S->noise >> 12) ^ (S->noise >> 11) ^ (S->noise >> 10) ^ 1) & 1;
        S->noise = (uint16_t)(S->noise * 2 + parity);
        int reload = 0x20000 >> (cnt >> 10 & 15); S->noise_timer += reload; if (S->noise_timer < 0) S->noise_timer += reload;
    }
    int32_t mixed[2] = {0}, echo[2] = {0}, previous = 0, captured[2] = {0};
    for (int n = 0; n < SPU_VOICES; n++) {
        ps1_spu_voice *v = &S->voice[n];
        if (S->key_off >> n & 1) { v->phase = RELEASE; v->env_counter = 0; }
        if (S->key_on >> n & 1) key_on(m, n);
        volume(&v->volume[0], REG(n * 16)); volume(&v->volume[1], REG(n * 16 + 2));
        int32_t sample = cnt & 0x8000 ? voice_sample(m, n, previous) : 0; previous = sample;
        if (n == 1 || n == 3) captured[n == 3] = sample;
        for (int ch = 0; ch < 2; ch++) {
            int32_t out = (sample * v->volume[ch].level) >> 15; mixed[ch] += out;
            if (bits(m, 0x198) >> n & 1) echo[ch] += out;
        }
        if (!(cnt & 0x8000)) { v->phase = OFF; v->env = 0; }
        adsr(m, n);
    }
    S->key_on = S->key_off = 0;
    // Overview / Memory layout: voice1/3 capture their post-ADSR mono samples even when inaudible; CD inputs are silent until external mixing is implemented
    if (REG(0x1ac) & 12) {
        ram_put(m, S->capture, 0); ram_put(m, 0x400 + S->capture, 0);
        ram_put(m, 0x800 + S->capture, (uint16_t)captured[0]); ram_put(m, 0xc00 + S->capture, (uint16_t)captured[1]);
    }
    S->capture = (S->capture + 2) & 0x3ff;
    int32_t wet[2]; reverb(m, echo, wet);
    for (int ch = 0; ch < 2; ch++) {
        volume(&S->main[ch], REG(0x180 + ch * 2));
        mixed[ch] = (clamp(mixed[ch] + wet[ch], -32768, 32767) * S->main[ch].level) >> 15;
        if (!(cnt & 0x4000)) mixed[ch] = 0;
    }
    if (S->audio_count == SPU_AUDIO_FRAMES) { S->audio_read = (S->audio_read + 1) % SPU_AUDIO_FRAMES; S->audio_count--; }
    unsigned at = (S->audio_read + S->audio_count) % SPU_AUDIO_FRAMES;
    S->audio[at * 2] = (int16_t)mixed[0]; S->audio[at * 2 + 1] = (int16_t)mixed[1]; S->audio_count++;
}
int spu_audio_read(ps1 *m, int16_t *out, int maxframes) {
    int n = maxframes > 0 && (unsigned)maxframes < S->audio_count ? maxframes : maxframes > 0 ? (int)S->audio_count : 0;
    for (int i = 0; i < n; i++) { unsigned at = (S->audio_read + (unsigned)i) % SPU_AUDIO_FRAMES; out[i * 2] = S->audio[at * 2]; out[i * 2 + 1] = S->audio[at * 2 + 1]; }
    S->audio_read = (S->audio_read + (unsigned)n) % SPU_AUDIO_FRAMES; S->audio_count -= (unsigned)n; return n;
}
uint16_t spu_read(ps1 *m, uint32_t off) {
    off &= 0x3fe;
    if (off == R_STAT) {
        uint16_t mode = REG(R_CNT) >> 4 & 3;
        return (uint16_t)(REG(R_STAT) | S->irq << 6 | (mode & 2 ? 0x80 : 0) | (mode == 2 ? 0x100 : mode == 3 ? 0x200 : 0) | (REG(0x1ac) & 12 && S->capture & 0x200 ? 0x800 : 0));
    }
    if (off == R_END || off == R_END + 2) return (uint16_t)(S->endx >> (off == R_END ? 0 : 16));
    if (off == 0x1b8 || off == 0x1ba) return (uint16_t)S->main[(off - 0x1b8) / 2].level;
    if (off >= 0x200 && off < 0x260) return (uint16_t)S->voice[(off - 0x200) / 4].volume[(off / 2) & 1].level;
    return REG(off);
}
void spu_write(ps1 *m, uint32_t off, uint16_t v) {
    off &= 0x3fe;
    if (off == R_END || off == R_END + 2 || off == R_STAT) return;
    REG(off) = v;
    if (off == R_ADDR) S->addr = (uint32_t)v * 8;
    else if (off == 0x1a2) S->reverb_addr = (uint32_t)v * 8;
    else if (off == R_DATA) {
        if (S->nfifo < 32) S->fifo[S->nfifo++] = v;
        else RED_ONCE(m, RED_SPU_EFFECT, "spu: CPU transfer FIFO overflow (sub-sample transfer timing is not modelled)");
    } else if (off == R_KON || off == R_KON + 2) {
        S->key_on |= (uint32_t)v << (off == R_KON ? 0 : 16); S->key_on &= 0xffffff;
        if (v) RED_ONCE(m, RED_SPU_VOICE, "spu: voice RAM fetch and key-on timing is sampled at 44.1 kHz; sub-sample timing is not modelled");
    } else if (off == R_KOFF || off == R_KOFF + 2) S->key_off |= (uint32_t)v << (off == R_KOFF ? 0 : 16);
    else if (off < 0x180 && (off & 15) == 12) S->voice[off / 16].env = (int16_t)v;
    else if (off == R_CNT) {
        if (!(v & 0x40)) S->irq = 0;
        if (v & 15) RED_ONCE(m, RED_SPU_EFFECT, "spu: CD/PIO input mixing is not modelled yet");
    } else if (off == 0x1ac && v != 4) RED_ONCE(m, RED_SPU_EFFECT, "spu: non-512KB RAM configuration is not modelled yet");
}
void spu_dma_write(ps1 *m, uint32_t w) { put(m, (uint16_t)w); put(m, (uint16_t)(w >> 16)); }
uint32_t spu_dma_read(ps1 *m) { uint32_t lo = get(m); return lo | (uint32_t)get(m) << 16; }
