// Native coverage must distinguish entry misses, overwritten bytes and delay-slot fallback, without counting BIOS startup as program execution. Synthetic code only.
#include "native.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static const uint32_t guest[] = {0x24040005, 0x03e00008, 0};
static const uint32_t runs[][2] = {{0x80010000, 0x8001000c}};
static void compiled(ps1 *m) {
    NATIVE_OP(addiu, 0x80010000u, 0x24040005u); NATIVE_OP(jr, 0x80010004u, 0x03e00008u); NATIVE_OP(sll, 0x80010008u, 0u);
}
static const ps1_native_fn functions[] = {{compiled, 0x80010000, runs, 1, guest}};
static const ps1_native_entry entries[] = {{0x80010000, 0}};
static const ps1_native_set code = {"coverage", functions, 1, entries, 1};

int main(void) {
    ps1 *m = ps1_new(); if (!m) return 1;
    ps1_coverage c = {.entry = 0x80010000}; m->coverage = &c;
    cpu_step(m);
    CHECK(!c.started && !c.interpreted[COVER_BIOS], "BIOS startup before the game entry is excluded");
    memcpy(m->ram + 0x10000, guest, sizeof guest); ps1_native_add(m, &code);
    m->cpu.pc = 0x80010000; m->cpu.next_pc = m->cpu.pc + 4; m->cpu.r[31] = 0x80010010;
    CHECK(native_run(m) && c.started && c.native[COVER_PROGRAM] == 3 && m->cpu.r[4] == 5, "native entry starts the program counters and records all three instructions");
    mem_write32(m, 0x80010000, 0x24040009);
    m->cpu.pc = 0x80010000; m->cpu.next_pc = m->cpu.pc + 4;
    CHECK(!native_run(m) && m->native_miss == FALLBACK_CHANGED, "overwritten native bytes have a distinct fallback reason"); cpu_step(m);
    CHECK(!native_run(m) && m->native_miss == FALLBACK_NO_ENTRY, "an interior PC without an entry is distinguished from changed bytes"); cpu_step(m);
    m->next_event = m->cycles + 1; cpu_run(m);
    CHECK(c.interpreted[COVER_PROGRAM] == 3 && m->cpu.pc == 0x80010010, "the dispatcher records the uncaptured delay slot after an interpreted branch");
    m->nnative = 0; m->cpu.pc = 0xbfc00000; m->cpu.next_pc = m->cpu.pc + 4; cpu_step(m);
    CHECK(c.interpreted[COVER_BIOS] == 1, "BIOS service instructions after entry are reported separately");
    CHECK(coverage_region(0xa0210000) == COVER_PROGRAM && coverage_region(0x80000100) == COVER_KERNEL && coverage_region(0x1f801000) == COVER_OTHER,
          "address regions normalize RAM mirrors and distinguish kernel, ROM and other space");
    char path[] = "/tmp/muyiunative-coverage-XXXXXX"; int fd = mkstemp(path);
    CHECK(fd >= 0, "a temporary report can be allocated"); if (fd >= 0) close(fd);
    CHECK(!ps1_coverage_write(&c, path), "coverage writes successfully");
    char report[4096] = {0}; FILE *f = fopen(path, "r"); if (f) { fread(report, 1, sizeof report - 1, f); fclose(f); } unlink(path);
    CHECK(strstr(report, "\"started\": true") && strstr(report, "\"program_ram\": {\"native\": 3, \"interpreted\": 3}"), "report contains exact counters, rather than rounded percentages");
    CHECK(strstr(report, "code_changed") && strstr(report, "no_entry") && strstr(report, "delay_slot") && strstr(report, "disabled"), "all observed fallback reasons survive export");
    CHECK(!strstr(report, "24040009") && !strstr(report, "words"), "report contains no instruction bytes");
    CHECK(ps1_coverage_write(&c, "/nonexistent/muyiunative/coverage.json") < 0, "report I/O errors fail");
    m->memory_watch = 1; mem_write32(m, 0x80200100, 7); mem_write16(m, 0x1f800002, 9);
    CHECK(m->memory_dirty[0] == 1 && m->scratch_dirty, "CPU stores mark mirrored RAM and scratchpad for block comparisons");
    memset(m->memory_dirty, 0, sizeof m->memory_dirty); mem_written(m, 0x801ffffe, 4);
    CHECK(m->memory_dirty[0] == 1 && m->memory_dirty[7] == (UINT64_C(1) << 63), "a tracked range crossing the RAM mirror boundary marks both pages");
    ps1_coverage_free(&c); m->coverage = NULL; ps1_free(m);
    printf("coverage: %d checks, %d failed\n", checks, fails); return fails != 0;
}
