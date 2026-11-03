// Native code against the interpreter (plan.md, Testing: each MIPS instruction checked in recompiled form against the interpreter). tools/tests/native_cases.py
// assembles a program that runs each kind of instruction (arithmetic and its overflows, MULT/DIV and their waits, every load and store with the load delay slot and
// LWL/LWR merging, each branch taken and not, calls through JAL, JALR and a jump table, the exceptions with a handler that returns from them, COP0, the GTE, a software
// interrupt, cache isolation, a loop) and generates its native code with tools/codegen.py. Two machines run it from the same state, one interpreting, one with
// the native code; their CPU state, cycles and memory must be equal each time the dispatcher stops, at the end of a block every 50 cycles or so. The runtime's
// decoder must also agree with the tools' on every opcode (plan.md, decision log: the tools' own decoder)
#include "native.h"
#include <stdlib.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fputs("FAIL: ", stderr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

extern const struct native_word { uint32_t word; int op; } native_words[];
extern const unsigned native_words_n, native_main_n, native_handler_n;
extern const uint32_t native_base, native_vector, native_main[], native_handler[];
extern const ps1_native_set native_test;

static ps1 *machine(int native) {
    ps1 *m = ps1_new();
    if (!m) { fputs("FAIL: no memory for the machine\n", stderr); exit(1); }
    memcpy(m->ram + (native_base & 0x1fffff), native_main, native_main_n * 4);
    memcpy(m->ram + (native_vector & 0x1fffff), native_handler, native_handler_n * 4);
    m->cpu.cop0[C0_SR] = 0; m->cpu.pc = native_base; m->cpu.next_pc = native_base + 4;
    if (native) ps1_native_add(m, &native_test);
    return m;
}

static int differ(const ps1 *a, const ps1 *b) {   // the first difference, on stderr
    const ps1_cpu *x = &a->cpu, *y = &b->cpu;
    for (int i = 0; i < 32; i++) if (x->r[i] != y->r[i]) { fprintf(stderr, "r%d %08x / %08x\n", i, x->r[i], y->r[i]); return 1; }
    for (int i = 0; i < 32; i++) if (x->cop0[i] != y->cop0[i]) { fprintf(stderr, "cop0 %d %08x / %08x\n", i, x->cop0[i], y->cop0[i]); return 1; }
    if (memcmp(x, y, sizeof *x)) { fprintf(stderr, "pc %08x / %08x, cycles %llu / %llu, other CPU state\n", x->pc, y->pc, (unsigned long long)a->cycles, (unsigned long long)b->cycles); return 1; }
    if (a->cycles != b->cycles) { fprintf(stderr, "cycles %llu / %llu\n", (unsigned long long)a->cycles, (unsigned long long)b->cycles); return 1; }
    for (unsigned i = 0; i < RAM_SIZE; i += 4) if (memcmp(a->ram + i, b->ram + i, 4)) { fprintf(stderr, "RAM at %06x\n", i); return 1; }
    return a->exit_requested != b->exit_requested || a->exit_code != b->exit_code;
}

int main(void) {
    for (unsigned i = 0; i < native_words_n; i++)
        CHECK(cpu_decode(native_words[i].word) == native_words[i].op, "%08x decodes as %d in the runtime, %d in tools/mips.py", native_words[i].word,
              cpu_decode(native_words[i].word), native_words[i].op);

    ps1 *a = machine(0), *b = machine(1);
    int stops = 0, same = 1;
    while (!(a->exit_requested && b->exit_requested) && stops < 100000 && same) {
        a->next_event = b->next_event = a->cycles + 50;
        cpu_run(a); cpu_run(b); stops++;
        if (differ(a, b)) { same = 0; fprintf(stderr, "  after %d stops, at %08x / %08x\n", stops, a->cpu.cur_pc, b->cpu.cur_pc); }
    }
    CHECK(same, "the native code and the interpreter agree at every stop");
    CHECK(a->exit_requested && a->exit_code == 0 && b->exit_requested, "the program reaches its exit port (%d stops, %llu cycles)", stops, (unsigned long long)a->cycles);
    CHECK(b->native_runs > 20, "the native code ran: entered %llu times", (unsigned long long)b->native_runs);
    CHECK(mem_read32(a, 0x8003001c) == 16 && mem_read32(a, 0x80030020) == 0x445566,
          "an IRQ at a generated GTE block issues GPF exactly once before the BIOS-style return skips it");
    // Eight synchronous exceptions return to JR RA and its delay slot; the IRQ returns past GPF into a block and interprets its nine remaining instructions.
    CHECK(b->interp_steps == 25, "the native machine interpreted %llu instructions of the interpreter's %llu", (unsigned long long)b->interp_steps,
          (unsigned long long)a->interp_steps);

    // The handler's log, CAUSE and EPC for each exception, in the order the program raises them
    static const uint32_t codes[] = {EXC_OV, EXC_OV, EXC_OV, EXC_ADEL, EXC_ADES, EXC_ADEL, EXC_SYS, EXC_BP, EXC_RI, EXC_CPU, EXC_SYS, EXC_INT};
    uint32_t log[2 * 16], n; memcpy(log, a->ram + 0x20100, sizeof log); memcpy(&n, a->ram + 0x20000, 4);
    n = (n - 0x80020100) / 8;
    CHECK(n == sizeof codes / sizeof *codes, "the handler logged %u exceptions", n);
    for (unsigned i = 0; i < n && i < sizeof codes / sizeof *codes; i++) CHECK((log[2 * i] >> 2 & 31) == codes[i], "exception %u: code %u", i, log[2 * i] >> 2 & 31);
    CHECK(n > 10 && log[2 * 10] >> 31 && (log[2 * 11] & 0x100), "SYSCALL in a delay slot sets CAUSE.BD; the software interrupt's bit is in CAUSE");

    fprintf(stderr, "native: %d checks, %d failed\n", checks, fails);
    ps1_free(a); ps1_free(b);
    return fails != 0;
}
