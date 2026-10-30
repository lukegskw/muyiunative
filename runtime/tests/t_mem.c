// The memory map (mem.c): the segments' views of physical memory, RAM through its 8 MB window, the scratchpad not through KSEG1, the BIOS as ROM, unaligned accesses,
// and what is not modelled yet. The [RED] lines it prints are the accesses to unmodelled space it makes on purpose
#include "ps1.h"
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

int main(void) {
    ps1 *m = ps1_new();
    if (!m) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }
    CHECK(ps1_phys(0x00001234) == 0x00001234, "KUSEG is physical memory");
    CHECK(ps1_phys(0x80001234) == 0x00001234, "KSEG0 is physical memory");
    CHECK(ps1_phys(0xbfc00180) == 0x1fc00180, "KSEG1 is physical memory");
    CHECK(ps1_phys(0xfffe0130) == 0xfffe0130, "KSEG2 is not translated");

    mem_write32(m, 0x80000100, 0x11223344);
    CHECK(mem_read32(m, 0x00000100) == 0x11223344 && mem_read32(m, 0xa0000100) == 0x11223344, "one RAM through KUSEG, KSEG0 and KSEG1");
    CHECK(mem_read32(m, 0x80200100) == 0x11223344 && mem_read32(m, 0x80600100) == 0x11223344, "RAM repeats every 2 MB through its 8 MB window");
    CHECK(mem_read8(m, 0x80000100) == 0x44 && mem_read16(m, 0x80000102) == 0x1122, "bytes and halfwords are little-endian");
    mem_write8(m, 0x80000101, 0xaa); mem_write16(m, 0x80000102, 0xbbcc);
    CHECK(mem_read32(m, 0x80000100) == 0xbbccaa44, "byte and halfword stores change only their bytes");
    mem_write32(m, 0x801ffffc, 0x55667788);
    CHECK(mem_read32(m, 0x001ffffc) == 0x55667788, "the last word of RAM");

    mem_write32(m, 0x1f800000, 0xcafef00d); mem_write32(m, 0x9f8003fc, 0x0badc0de);
    CHECK(mem_read32(m, 0x9f800000) == 0xcafef00d && mem_read32(m, 0x1f8003fc) == 0x0badc0de, "the scratchpad through KUSEG and KSEG0");
    CHECK(mem_read32(m, 0xbf800000) == 0, "no scratchpad through KSEG1");
    CHECK(mem_read32(m, 0x1f800400) == 0, "nothing after the scratchpad's 1 KB");

    memcpy(m->bios, "\x0d\xf0\xad\x0b", 4); memcpy(m->bios + BIOS_SIZE - 4, "\x78\x56\x34\x12", 4);
    CHECK(mem_read32(m, 0x1fc00000) == 0x0badf00d && mem_read32(m, 0x9fc00000) == 0x0badf00d && mem_read32(m, 0xbfc00000) == 0x0badf00d, "the BIOS through KUSEG, KSEG0 and KSEG1");
    CHECK(mem_read32(m, 0xbfc7fffc) == 0x12345678, "the last word of the BIOS");
    mem_write32(m, 0xbfc00000, 0); mem_write8(m, 0x9fc00001, 0);
    CHECK(mem_read32(m, 0xbfc00000) == 0x0badf00d, "the BIOS is ROM: stores are dropped");

    CHECK(mem_read32(m, 0x00800000) == 0, "nothing after RAM's 8 MB window");
    CHECK(mem_read32(m, 0x20000000) == 0, "nothing in KUSEG above 512 MB");
    mem_write32(m, 0x80000100, 0x11223344);
    CHECK(mem_read32(m, 0x80000101) == 0 && mem_read16(m, 0x80000101) == 0, "an unaligned read reaches no memory");
    mem_write32(m, 0x80000102, 0xffffffff);
    CHECK(mem_read32(m, 0x80000100) == 0x11223344, "an unaligned store changes nothing");

    // The hardware logs of ps1-tests cpu/io-access-bitwidth and cpu/code-in-io
    mem_write8(m, 0x1f801080, 0x12345678); mem_write16(m, 0x1f8010f0, 0x12345678); mem_write8(m, 0x1f801108, 0x12345678);
    CHECK(mem_read32(m, 0x1f801080) == 0x345678 && mem_read32(m, 0x1f8010f0) == 0x12345678 && mem_read16(m, 0x1f801108) == 0x5678,
          "a narrow store to an I/O port drives the whole register: DMA0's address, DPCR, a timer target");
    mem_write16(m, 0x1f801800, 0x5678);
    CHECK((mem_read8(m, 0x1f801800) & 3) == 2, "the CD-ROM takes a 16-bit store as two bytes at one address: the index ends at 56h & 3");
    mem_write8(m, 0x1f801800, 0);
    uint32_t op = 0;
    mem_write32(m, 0x1f801084, 0x03e00008);
    CHECK(mem_fetch(m, 0x1f801084, &op) && op == 0x03e00008, "code runs from the DMA registers");
    CHECK(!mem_fetch(m, 0x1f801070, &op) && !mem_fetch(m, 0x1f801820, &op) && !mem_fetch(m, 0x1f800000, &op),
          "fetching from the interrupt controller, the MDEC or the scratchpad is a bus error");

    printf("mem: %d checks, %d failed\n", checks, fails);
    ps1_free(m);
    return fails != 0;
}
