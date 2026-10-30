// Side-loading a PS-X EXE (plan.md, phase 2: homebrew test programs). psx-spx, CDROM File Formats, "FILENAME.EXE (General-Purpose Executable)": an 800h-byte header
// (initial PC, GP, destination, size, BSS, SP base/offset), then the code/data copied to the destination; R4=1, R5=0; SP 0 keeps the caller's stack. The retail BIOS and
// OpenBIOS start their shell at 80030000h after the kernel is set up (psx-spx, Kernel (BIOS); observed: OpenBIOS first runs RAM code there, about 1.6M cycles after reset),
// so the EXE replaces the shell at that point, as a boot executable would
#include "ps1.h"
#include <stdlib.h>
#include <string.h>

enum { SHELL = 0x80030000, HDR = 0x800 };

static uint32_t word(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int in_ram(uint32_t a, uint32_t n) { uint32_t p = ps1_phys(a); return p < RAM_SIZE && n <= RAM_SIZE - p; }

int ps1_load_exe(ps1 *m, const uint8_t *exe, size_t n) {
    if (n < HDR || memcmp(exe, "PS-X EXE", 8)) { RED("exe: not a PS-X EXE"); return -1; }
    uint32_t pc = word(exe + 0x10), dst = word(exe + 0x18), size = word(exe + 0x1c);
    if (size > n - HDR || !in_ram(dst, size) || !in_ram(pc, 4)) { RED("exe: the header's code/data does not fit main RAM"); return -1; }
    uint8_t *copy = malloc(n);
    if (!copy) { RED("exe: no memory"); return -1; }
    memcpy(copy, exe, n); free(m->exe); m->exe = copy; m->exe_n = n;
    return 0;
}

int ps1_exe_due(const ps1 *m) { return m->exe && m->cpu.pc == SHELL; }

void ps1_exe_start(ps1 *m) {
    const uint8_t *h = m->exe; ps1_cpu *c = &m->cpu;
    uint32_t dst = word(h + 0x18), size = word(h + 0x1c), bss = word(h + 0x28), bss_n = word(h + 0x2c), sp = word(h + 0x30);
    memcpy(m->ram + ps1_phys(dst), h + HDR, size);
    if (bss_n && in_ram(bss, bss_n)) memset(m->ram + ps1_phys(bss), 0, bss_n);
    c->pc = word(h + 0x10); c->next_pc = c->pc + 4; c->ld_r = 0; c->branch = c->delay = 0; c->boundary = 1;
    c->r[28] = word(h + 0x14); c->r[4] = 1; c->r[5] = 0;
    if (sp) c->r[29] = c->r[30] = sp + word(h + 0x34);
    free(m->exe); m->exe = NULL; m->exe_n = 0;
}
