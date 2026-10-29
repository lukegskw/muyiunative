// The CD-ROM drive with no disc (cdrom.c; psx-spx, CDROM Drive): the status register, Init's two responses, GetID's "no disc" answer, the error for a command that needs a
// disc, Test 20h's date and version, an unknown command and a wrong parameter count, a command waiting while an interrupt is unacknowledged, and IRQ 2. Time passes event
// by event, as cpu_run and sched_run make it pass
#include "ps1.h"
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

enum { HSTS = 0x1f801800, CMD = 0x1f801801, PARAM = 0x1f801802, HINT = 0x1f801803 };
static ps1 *m;
static void send(uint8_t c, const uint8_t *p, int n) {
    mem_write8(m, HSTS, 0);
    for (int i = 0; i < n; i++) mem_write8(m, PARAM, p[i]);
    mem_write8(m, CMD, c);
}
static int wait_int(uint64_t *took) {   // the interrupt type of the next response, and the cycles it took
    uint64_t t0 = m->cycles;
    mem_write8(m, HSTS, 1);
    for (int i = 0; i < 100000 && !(mem_read8(m, HINT) & 7); i++) { m->cycles = m->next_event; sched_run(m); }
    if (took) *took = m->cycles - t0;
    return mem_read8(m, HINT) & 7;
}
static int response(uint8_t *out) { int n = 0; while (n < 16 && mem_read8(m, HSTS) & 0x20) out[n++] = mem_read8(m, CMD); return n; }
static void ack(void) { mem_write8(m, HSTS, 1); mem_write8(m, HINT, 0x1f); }

int main(void) {
    m = ps1_new();
    if (!m) { fputs("FAIL: no memory for the machine\n", stderr); return 1; }
    uint8_t r[16]; uint64_t took;
    mem_write8(m, HSTS, 1); mem_write8(m, PARAM, 0x1f); m->i_mask = 1u << IRQ_CDROM;
    CHECK(mem_read8(m, HSTS) == 0x19 && mem_read8(m, HINT) == 0xe0, "HSTS: bank 1, parameters empty and not full; HINTSTS reads its unused bits as 1");

    send(0x0a, NULL, 0);
    CHECK(mem_read8(m, HSTS) >> 7, "busy while a command runs");
    CHECK(wait_int(&took) == 3 && took == 0x13cce && response(r) == 1 && m->i_stat >> IRQ_CDROM & 1, "Init: INT3 with the status, after 13CCEh cycles, IRQ 2");
    ack();
    CHECK(wait_int(&took) == 2 && took == 0x4a00 && response(r) == 1, "Init: INT2 once INT3 is acknowledged");
    ack();

    send(0x1a, NULL, 0);
    CHECK(wait_int(NULL) == 3, "GetID: INT3 first");
    ack();
    static const uint8_t no_disc[8] = {0x08, 0x40};
    CHECK(wait_int(NULL) == 5 && response(r) == 8 && !memcmp(r, no_disc, 8), "GetID with no disc: INT5 08h 40h 00h...");
    ack();

    send(0x02, (const uint8_t[]){0x00, 0x02, 0x00}, 3);
    CHECK(wait_int(NULL) == 5 && response(r) == 2 && r[0] & 1 && r[1] == 0x80, "Setloc needs a disc: INT5 with error 80h");
    ack();
    send(0x19, (const uint8_t[]){0x20}, 1);
    CHECK(wait_int(NULL) == 3 && response(r) == 4 && !memcmp(r, "\x97\x01\x10\xc2", 4), "Test 20h: the controller's date and version");
    ack();
    send(0x55, NULL, 0);
    CHECK(wait_int(NULL) == 5 && response(r) == 2 && r[1] == 0x40, "an unknown command: error 40h");
    ack();
    send(0x0e, NULL, 0);
    CHECK(wait_int(NULL) == 5 && response(r) == 2 && r[1] == 0x20, "Setmode without its parameter: error 20h");
    ack();

    send(0x01, NULL, 0);
    CHECK(wait_int(NULL) == 3, "Nop: INT3");
    send(0x01, NULL, 0);
    for (int i = 0; i < 100; i++) { m->cycles = m->next_event; sched_run(m); }
    mem_write8(m, HSTS, 1);
    CHECK((mem_read8(m, HINT) & 7) == 3 && mem_read8(m, HSTS) >> 7, "a command waits while INT3 is unacknowledged, busy");
    ack();
    CHECK(wait_int(NULL) == 3, "then runs");
    ack();

    printf("cdrom: %d checks, %d failed\n", checks, fails);
    ps1_free(m);
    return fails != 0;
}
