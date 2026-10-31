// Phase 4 (plan.md): transient code versions remain native after being overwritten and reloaded; unknown bytes fall back to the interpreter. The capture preserves
// both versions, and writes into executing code, including a RAM mirror and synchronous DMA (psx-spx, Memory Map/DMA Channels), cannot execute stale constants.
#include "native.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
extern const ps1_native_set native_overlays;
extern const uint32_t overlay_case_0[], overlay_case_1[], overlay_case_2[], overlay_case_3[];

static void enter(ps1 *m, uint32_t pc, const uint32_t *words, size_t n) {
    memcpy(m->ram + (pc & 0x1fffff), words, n * 4);
    m->cpu.pc = pc; m->cpu.next_pc = pc + 4; m->cpu.r[31] = 0x80020000; m->cpu.boundary = 1; m->cpu.cop0[C0_SR] = 0;
}

int main(void) {
    ps1 *a = ps1_new(), *b = ps1_new();
    if (!a || !b) return 1;
    ps1_native_add(b, &native_overlays);
    const uint32_t *versions[] = {overlay_case_0, overlay_case_1, overlay_case_0};
    ps1_capture c; ps1_capture_start(a, &c);
    for (int i = 0; i < 3; i++) {
        enter(a, 0x80010000, versions[i], 3); enter(b, 0x80010000, versions[i], 3);
        for (int j = 0; j < 3; j++) cpu_step(a);
        CHECK(native_run(b), "a captured version runs natively, including the older version reloaded later");
        CHECK(!memcmp(&a->cpu, &b->cpu, sizeof a->cpu) && a->cycles == b->cycles, "each version agrees with the interpreter, registers, delay slots and cycles");
    }
    char dir[] = "/tmp/ps1native-overlays-XXXXXX", path[256], prefix[256];
    CHECK(mkdtemp(dir), "temporary capture directory"); snprintf(prefix, sizeof prefix, "%s/cap", dir);
    CHECK(!ps1_capture_write(a, &c, prefix), "capture writes all observed versions");
    CHECK(!a->on_step && !a->step_ctx, "writing a capture detaches its callback before releasing the records");
    snprintf(path, sizeof path, "%s.blocks", prefix); FILE *f = fopen(path, "rb"); char magic[8]; unsigned records = 0; uint64_t hits = 0;
    CHECK(f && fread(magic, 1, 8, f) == 8 && !memcmp(magic, "PS1BLK1", 8), "versioned block format");
    uint32_t pc, n, words[258]; uint64_t count;
    while (f && fread(&pc, 4, 1, f) == 1) {
        if (fread(&n, 4, 1, f) != 1 || fread(&count, 8, 1, f) != 1 || n > 258 || fread(words, 4, n, f) != n) break;
        CHECK(pc == 0x80010000 && n == 3 && (!memcmp(words, overlay_case_0, 12) || !memcmp(words, overlay_case_1, 12)), "capture retains exact bytes even after an overlay was overwritten");
        records++; hits += count;
    }
    if (f) fclose(f);
    CHECK(records == 2 && hits == 3, "capture deduplicates repeated versions and counts executions");
    const char *suffix[] = {".txt", ".ram", ".blocks"};
    for (int i = 0; i < 3; i++) { snprintf(path, sizeof path, "%s%s", prefix, suffix[i]); unlink(path); } rmdir(dir);

    uint32_t unknown[] = {0x24020021, 0x03e00008, 0}; enter(b, 0x80010000, unknown, 3);
    uint64_t cycles = b->cycles;
    CHECK(!native_run(b) && b->cycles == cycles, "unknown bytes do not enter any native version or advance time");
    cpu_step(b); CHECK(b->cpu.r[2] == 33, "the interpreter covers the unseen version");

    ps1_reset(a); ps1_reset(b); memset(a->ram, 0, RAM_SIZE); memset(b->ram, 0, RAM_SIZE);
    enter(a, 0x80011000, overlay_case_2, 8); enter(b, 0x80011000, overlay_case_2, 8);
    a->next_event = b->next_event = 8;
    cpu_run(a); cpu_run(b);
    CHECK(!memcmp(&a->cpu, &b->cpu, sizeof a->cpu) && a->cycles == b->cycles && b->cpu.r[2] == 44,
          "a native store through a RAM mirror changes the very next instruction, as in the interpreter");

    ps1_reset(a); ps1_reset(b); memset(a->ram, 0, RAM_SIZE); memset(b->ram, 0, RAM_SIZE);
    enter(a, 0x80012000, overlay_case_3, 4); enter(b, 0x80012000, overlay_case_3, 4);
    ps1 *machines[] = {a, b};
    for (int i = 0; i < 2; i++) {
        ps1 *m = machines[i]; m->dpcr |= 1u << 27; m->dma[6].madr = 0x12004; m->dma[6].bcr = 1;
        m->cpu.r[8] = 0x1f8010e8; m->cpu.r[9] = 0x11000002; m->next_event = 3;
    }
    cpu_run(a); cpu_run(b);
    CHECK(!memcmp(&a->cpu, &b->cpu, sizeof a->cpu) && a->cycles == b->cycles && a->cpu.pc == 0x80000080,
          "DMA into executing code returns to the dispatcher before its overwritten instruction raises a reserved-instruction exception");
    CHECK(!memcmp(a->ram, b->ram, RAM_SIZE), "self-modification and DMA leave identical RAM");
    ps1_capture_start(a, &c); enter(a, 0x80010000, overlay_case_0, 3);
    for (int i = 0; i < 3; i++) cpu_step(a);
    CHECK(ps1_capture_write(a, &c, prefix) == -1 && !a->on_step && !c.a,
          "a failed capture write releases its records and detaches the callback");
    ps1_free(a); ps1_free(b);
    printf("overlays: %d checks, %d failed\n", checks, fails); return fails != 0;
}
