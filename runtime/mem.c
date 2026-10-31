// The memory map as the CPU sees it (psx-spx, Memory Map; I/O Map): KUSEG, KSEG0 and KSEG1 are views of one 512 MB physical space, and KSEG2 holds only the cache control
// port. Main RAM (2 MB) repeats through the first 8 MB (Memory Mirrors: the BIOS's default RAM size setting), the scratchpad (1 KB) is reached through KUSEG and KSEG0 but
// not KSEG1, and the BIOS (512 KB) is ROM, so a store to it is dropped. The I/O ports go to their devices; the expansion port has nothing plugged in (reads give FFh).
// Also here: the interrupt controller (I_STAT, I_MASK), the memory control registers (kept, without effect), and the debug console of the expansion region
#include "ps1.h"
#include <string.h>

enum { RAM_WINDOW = 0x00800000, EXP1_BASE = 0x1f000000, EXP1_SIZE = 0x00800000, SCRATCH_BASE = 0x1f800000, EXP2_BASE = 0x1f802000, BIOS_BASE = 0x1fc00000 };

// KSEG0 (cached) and KSEG1 (uncached) drop their top 3 bits. KUSEG is physical as it is in its first 512 MB, and above them reaches nothing (a bus error); KSEG2 has no physical view
uint32_t ps1_phys(uint32_t a) { return a >> 29 == 4 || a >> 29 == 5 ? a & 0x1fffffff : a; }

static uint8_t *at(ps1 *m, uint32_t a) {   // the byte at a virtual address in one of the machine's memories, NULL elsewhere
    uint32_t p = ps1_phys(a);
    if (p < RAM_WINDOW) return m->ram + p % RAM_SIZE;
    if (p - SCRATCH_BASE < SCRATCH_SIZE && a >> 29 != 5) return m->scratch + (p - SCRATCH_BASE);
    if (p - BIOS_BASE < BIOS_SIZE) return m->bios + (p - BIOS_BASE);
    return NULL;
}

static void unmapped(ps1 *m, const char *what, uint32_t a, unsigned n) {   // the first 32 only: a loop over unmapped space would flood the log
    if (m->unmapped++ < 32) RED("mem: %u-byte %s at %08x (pc %08x): nothing modelled there", n, what, a, m->cpu.cur_pc);
    else if (m->unmapped == 33) RED("mem: more unmapped accesses, no longer reported");
}

void irq_update(ps1 *m) {
    uint32_t *cause = &m->cpu.cop0[C0_CAUSE];
    *cause = m->i_stat & m->i_mask ? *cause | 0x400 : *cause & ~0x400u;
}
void irq_raise(ps1 *m, int n) { m->i_stat |= 1u << n; irq_update(m); }

void tty_putc(ps1 *m, char c) {   // one line at a time, to the machine's sink or to stderr
    if (c == '\n' || m->ntty == (int)sizeof m->tty - 1) {
        m->tty[m->ntty] = 0; m->ntty = 0;
        if (m->tty_out) m->tty_out(m->tty_ctx, m->tty); else LOG("tty: %s", m->tty);
        if (c == '\n') return;
    }
    if (c != '\r') m->tty[m->ntty++] = c;
}

// The expansion region 2 (psx-spx, Expansion Port): POST is the boot progress display; 1F802080h..82h are the debug console, break and exit ports that psx-spx documents as
// the "PCSX-Redux Emulation Expansion", so test programs can print and end a run. Nothing else is fitted (no DUART, no development board): stores are dropped
static void exp2_write(ps1 *m, uint32_t p, uint32_t v) {
    if (p == 0x1f802041) m->post = v & 0xff;
    else if (p == 0x1f802080) tty_putc(m, (char)v);
    else if (p == 0x1f802081) LOG("debug: break at %08x", m->cpu.cur_pc);
    else if (p == 0x1f802082) { m->exit_requested = 1; m->exit_code = (int16_t)v; }
}

// The I/O ports by physical address; n is the access size in bytes. Registers are 32-bit unless the device's are narrower (CD-ROM: 8-bit, SPU: 16-bit, controller port:
// mixed); a narrower read sees its part of the register, and a narrower store is described at wr
static uint32_t io_read(ps1 *m, uint32_t p, unsigned n) {
    if (p >= 0x1f801c00 && p < 0x1f802000) {
        uint32_t lo = spu_read(m, (p & ~1u) - 0x1f801c00);
        return n == 4 ? lo | (uint32_t)spu_read(m, p + 2 - 0x1f801c00) << 16 : lo >> (p & 1) * 8;
    }
    if (p >= 0x1f801800 && p < 0x1f801804) { uint32_t v = 0; for (unsigned i = 0; i < n; i++) v |= (uint32_t)cd_read(m, p - 0x1f801800) << 8 * i; return v; }
    if (p >= 0x1f801040 && p < 0x1f801060) return sio_read(m, p - 0x1f801040, n);
    uint32_t a = p & ~3u, v;
    if (a >= 0x1f801000 && a < 0x1f801024) v = m->memctl[(a - 0x1f801000) / 4];
    else if (a == 0x1f801060) v = m->ram_size;
    else if (a == 0x1f801070) v = m->i_stat;
    else if (a == 0x1f801074) v = m->i_mask;
    else if (a >= 0x1f801080 && a < 0x1f801100) v = dma_read(m, a - 0x1f801080);
    else if (a >= 0x1f801100 && a < 0x1f801130) v = tmr_read(m, a - 0x1f801100);
    else if (a == 0x1f801810) v = gpu_read(m);
    else if (a == 0x1f801814) v = gpu_stat(m);
    else if (a == 0x1f801820 || a == 0x1f801824) { v = mdec_read(m, a - 0x1f801820); dma_resume(m); }
    else if (a >= EXP2_BASE && a < EXP2_BASE + 0x1000) v = 0xffffffff;
    else if (a == 0xfffe0130) v = m->cache_ctl;
    else { unmapped(m, "read", p, n); return 0; }
    return v >> (p & 3) * 8;
}

static void io_write(ps1 *m, uint32_t p, uint32_t v, unsigned n) {   // v is the whole CPU register, whatever n is (wr)
    if (p >= 0x1f801c00 && p < 0x1f802000) {
        spu_write(m, (p & ~1u) - 0x1f801c00, (uint16_t)v);
        if (n == 4) spu_write(m, p + 2 - 0x1f801c00, (uint16_t)(v >> 16));
        return;
    }
    // The CD-ROM's 8-bit bus takes a wider store as its bytes, low first, all at the same address: the hardware log of ps1-tests cpu/io-access-bitwidth shows the index
    // register at 2 after a 16-bit store of 5678h or a 32-bit store of 12345678h to 1F801800h
    if (p >= 0x1f801800 && p < 0x1f801804) { for (unsigned i = 0; i < n; i++) cd_write(m, p - 0x1f801800, (uint8_t)(v >> 8 * i)); return; }
    uint32_t bytes = n == 4 ? v : v & ((1u << 8 * n) - 1);   // the store's own bytes
    if (p >= 0x1f801040 && p < 0x1f801060) { sio_write(m, p - 0x1f801040, bytes, n); return; }
    if (p >= EXP2_BASE && p < EXP2_BASE + 0x1000) { exp2_write(m, p, bytes); return; }
    uint32_t a = p & ~3u;
    if (p & 3) v = bytes << (p & 3) * 8;   // into the byte lanes it names; what the hardware does here is not documented
    if (a >= 0x1f801000 && a < 0x1f801024) m->memctl[(a - 0x1f801000) / 4] = v;
    else if (a == 0x1f801060) m->ram_size = v;
    else if (a == 0x1f801070) { m->i_stat &= v; irq_update(m); }   // a 0 bit acknowledges its IRQ
    else if (a == 0x1f801074) { m->i_mask = v & 0x7ff; irq_update(m); }
    else if (a >= 0x1f801080 && a < 0x1f801100) dma_write(m, a - 0x1f801080, v);
    else if (a >= 0x1f801100 && a < 0x1f801130) tmr_write(m, a - 0x1f801100, v);
    else if (a == 0x1f801810) gpu_gp0(m, v);
    else if (a == 0x1f801814) gpu_gp1(m, v);
    else if (a == 0x1f801820 || a == 0x1f801824) { mdec_write(m, a - 0x1f801820, v); dma_resume(m); }
    else if (a == 0xfffe0130) m->cache_ctl = v;
    else unmapped(m, "write", p, n);
}

static uint32_t rd(ps1 *m, uint32_t a, unsigned n) {
    if (a % n) { RED("mem: %u-byte read at %08x: unaligned, reads 0", n, a); return 0; }   // the CPU raises its address error before this
    const uint8_t *s = at(m, a); uint32_t v = 0;
    if (s) { memcpy(&v, s, n); return v; }
    uint32_t p = ps1_phys(a), mask = n == 4 ? 0xffffffff : (1u << 8 * n) - 1;
    if (p - EXP1_BASE < EXP1_SIZE) return mask;
    return io_read(m, p, n) & mask;
}

// A store narrower than its port still drives the whole CPU register onto the bus, and a port it is aligned to takes all of it: the hardware log of ps1-tests
// cpu/io-access-bitwidth shows an 8-bit store of 12345678h leaving 345678h in DMA0's address, 12345678h in DPCR, 5678h in a timer target and in SPUCNT. RAM and the
// scratchpad take only the store's bytes
static void wr(ps1 *m, uint32_t a, uint32_t v, unsigned n) {
    if (a % n) { RED("mem: %u-byte write of %08x at %08x: unaligned, dropped", n, v, a); return; }
    uint32_t p = ps1_phys(a);
    if (p - BIOS_BASE < BIOS_SIZE || p - EXP1_BASE < EXP1_SIZE) return;   // ROM, and the empty expansion port
    uint8_t *d = at(m, a);
    if (d) { memcpy(d, &v, n); if (m->native_active) native_written(m, a, n); } else io_write(m, p, v, n);
}

uint8_t mem_read8(ps1 *m, uint32_t a) { return (uint8_t)rd(m, a, 1); }
uint16_t mem_read16(ps1 *m, uint32_t a) { return (uint16_t)rd(m, a, 2); }
uint32_t mem_read32(ps1 *m, uint32_t a) { return rd(m, a, 4); }
void mem_write8(ps1 *m, uint32_t a, uint32_t v) { wr(m, a, v, 1); }
void mem_write16(ps1 *m, uint32_t a, uint32_t v) { wr(m, a, v, 2); }
void mem_write32(ps1 *m, uint32_t a, uint32_t v) { wr(m, a, v, 4); }

// RAM and BIOS; the scratchpad is not executable (psx-spx, Scratchpad). The hardware log of ps1-tests cpu/code-in-io shows code running from the DMA and SPU registers,
// and a bus error from the interrupt controller and the MDEC; the other ports are not known, so they give the bus error too
int mem_fetch(ps1 *m, uint32_t a, uint32_t *op) {
    uint32_t p = ps1_phys(a);
    if (p < RAM_WINDOW) { memcpy(op, m->ram + p % RAM_SIZE, 4); return 1; }
    if (p - BIOS_BASE < BIOS_SIZE) { memcpy(op, m->bios + (p - BIOS_BASE), 4); return 1; }
    if (p - 0x1f801080 < 0x80 || p - 0x1f801c00 < 0x400) { *op = io_read(m, p, 4); return 1; }
    return 0;
}

// A load instruction halts the CPU until its data arrives; a store goes to the write queue and takes one cycle (psx-spx, CPU Specifications, Load and Store
// instructions). The cycles a load takes, its instruction's included, are the hardware log of ps1-tests cpu/access-time rounded: a console timing 100 loads against
// 100 nops, with the BIOS's memory control settings (psx-spx, Memory Control). Main RAM takes 5 whatever the width, the scratchpad 1, the internal ports 3 (measured
// 2.9 to 3.8), and the 8- and 16-bit buses one access per byte or halfword. Not modelled: other memory control settings, and the write queue filling up
unsigned mem_load_cycles(uint32_t a, unsigned n) {
    static const uint8_t bios[3] = {8, 13, 25}, exp1[3] = {7, 14, 26}, exp2[3] = {11, 26, 56}, exp3[3] = {7, 6, 10}, cdrom[3] = {8, 14, 26}, spu[3] = {18, 18, 39};
    uint32_t p = ps1_phys(a), w = n == 4 ? 2 : n == 2;
    if (p < RAM_WINDOW) return 5;
    if (p - EXP1_BASE < EXP1_SIZE) return exp1[w];
    if (p - SCRATCH_BASE < SCRATCH_SIZE) return 1;
    if (p - 0x1f801800 < 4) return cdrom[w];
    if (p - 0x1f801c00 < 0x400) return spu[w];
    if (p - 0x1f801000 < 0x1000) return 3;
    if (p - EXP2_BASE < 0x2000) return exp2[w];
    if (p - 0x1fa00000 < 0x200000) return exp3[w];
    if (p - BIOS_BASE < BIOS_SIZE) return bios[w];
    return a == 0xfffe0130 && n > 1 ? 2 : 1;   // the cache control port: 0.95, 1.9 and 1.9
}

const uint8_t *mem_code(ps1 *m, uint32_t a, uint32_t n) {
    uint32_t p = ps1_phys(a);
    if (p < RAM_WINDOW) { p %= RAM_SIZE; return n <= RAM_SIZE - p ? m->ram + p : NULL; }
    if (p - BIOS_BASE < BIOS_SIZE) return n <= BIOS_SIZE - (p - BIOS_BASE) ? m->bios + (p - BIOS_BASE) : NULL;
    return NULL;
}
