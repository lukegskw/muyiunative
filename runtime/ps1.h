// The console the runtime models (plan.md, section 2): its memories now, the CPU and the devices from phase 1 on. All of a machine's state is in its ps1 struct and
// nothing is global, so two machines can run side by side (the lockstep comparison, plan.md section 6). Sizes from psx-spx, Memory Map
#pragma once
#include <stdint.h>
#include <stdio.h>

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the console and the host are both little-endian: memory is read and written as host integers");

enum { RAM_SIZE = 2 << 20, SCRATCH_SIZE = 1 << 10, BIOS_SIZE = 512 << 10 };

typedef struct ps1 {
    uint8_t ram[RAM_SIZE];           // main RAM; the BIOS kernel keeps the first 64 KB
    uint8_t scratch[SCRATCH_SIZE];   // the scratchpad: the R3000A's data cache, used as fast RAM
    uint8_t bios[BIOS_SIZE];         // the BIOS ROM
} ps1;

ps1 *ps1_new(void);   // a machine with its memories zeroed; NULL when out of memory
void ps1_free(ps1 *m);

// Memory as the CPU sees it, by virtual address (mem.c). Alignment is the CPU's to check (an address error); an unaligned access here reaches nothing
uint32_t ps1_phys(uint32_t addr);
uint8_t mem_read8(ps1 *m, uint32_t addr);
uint16_t mem_read16(ps1 *m, uint32_t addr);
uint32_t mem_read32(ps1 *m, uint32_t addr);
void mem_write8(ps1 *m, uint32_t addr, uint8_t v);
void mem_write16(ps1 *m, uint32_t addr, uint16_t v);
void mem_write32(ps1 *m, uint32_t addr, uint32_t v);

// One line on stderr; a line that marks something unimplemented or wrong starts with "[RED] " (AGENTS.md)
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define RED(...) do { fputs("[RED] ", stderr); LOG(__VA_ARGS__); } while (0)
