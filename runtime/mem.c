// The memory map as the CPU sees it (psx-spx, Memory Map): KUSEG, KSEG0 and KSEG1 are views of one 512 MB physical space, and KSEG2 holds only the cache control port.
// Main RAM (2 MB) repeats through the first 8 MB (Memory Mirrors: the BIOS's default RAM size setting), the scratchpad (1 KB) is reached through KUSEG and KSEG0 but not KSEG1,
// and the BIOS (512 KB) is ROM, so a store to it is dropped. Nothing else is modelled yet (I/O ports, the expansion regions, the cache control port; phase 1): a read there
// gives 0 and a store is dropped, each with a [RED] line
#include "ps1.h"
#include <stdlib.h>
#include <string.h>

enum { RAM_WINDOW = 0x00800000, SCRATCH_BASE = 0x1f800000, BIOS_BASE = 0x1fc00000 };

ps1 *ps1_new(void) { return calloc(1, sizeof(ps1)); }
void ps1_free(ps1 *m) { free(m); }

// KSEG0 (cached) and KSEG1 (uncached) drop their top 3 bits. KUSEG is physical as it is in its first 512 MB, and above them reaches nothing (a bus error); KSEG2 has no physical view
uint32_t ps1_phys(uint32_t a) { return a >> 29 == 4 || a >> 29 == 5 ? a & 0x1fffffff : a; }

static uint8_t *at(ps1 *m, uint32_t a) {   // the byte at a virtual address in one of the machine's memories, NULL elsewhere
    uint32_t p = ps1_phys(a);
    if (p < RAM_WINDOW) return m->ram + p % RAM_SIZE;
    if (p - SCRATCH_BASE < SCRATCH_SIZE && a >> 29 != 5) return m->scratch + (p - SCRATCH_BASE);
    if (p - BIOS_BASE < BIOS_SIZE) return m->bios + (p - BIOS_BASE);
    return NULL;
}

static uint32_t rd(ps1 *m, uint32_t a, unsigned n) {
    const uint8_t *s = a % n ? NULL : at(m, a); uint32_t v = 0;
    if (s) memcpy(&v, s, n); else RED("mem: %u-byte read at %08x: %s, reads 0", n, a, a % n ? "unaligned" : "nothing modelled there");
    return v;
}

static void wr(ps1 *m, uint32_t a, uint32_t v, unsigned n) {
    if (ps1_phys(a) - BIOS_BASE < BIOS_SIZE) return;   // ROM
    uint8_t *d = a % n ? NULL : at(m, a);
    if (d) memcpy(d, &v, n); else RED("mem: %u-byte write of %08x at %08x: %s, dropped", n, v, a, a % n ? "unaligned" : "nothing modelled there");
}

uint8_t mem_read8(ps1 *m, uint32_t a) { return (uint8_t)rd(m, a, 1); }
uint16_t mem_read16(ps1 *m, uint32_t a) { return (uint16_t)rd(m, a, 2); }
uint32_t mem_read32(ps1 *m, uint32_t a) { return rd(m, a, 4); }
void mem_write8(ps1 *m, uint32_t a, uint8_t v) { wr(m, a, v, 1); }
void mem_write16(ps1 *m, uint32_t a, uint16_t v) { wr(m, a, v, 2); }
void mem_write32(ps1 *m, uint32_t a, uint32_t v) { wr(m, a, v, 4); }
