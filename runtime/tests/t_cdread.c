// Phase 2 data reads through the CD-ROM ports and DMA3, using raw sectors built here in /tmp, never game files. psx-spx, CDROM Drive: ReadN/ReadS, HCHPCTL/RDDATA,
// Incoming Data / Buffer Overrun Timings, Response Timings, Setloc/Read/Pause, MotorOn/Stop, GetID, GetlocL/P, GetTN/GetTD; CDROM Format: Mode1 and Mode2/Form1. An acknowledged response precedes
// next interrupt, and BFRD locks the requested data independently of later sectors: https://psx-spx.consoledev.net/cdromdrive/
#include "ps1.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
enum { HSTS = 0x1f801800, CMD = HSTS + 1, DATA = HSTS + 2, HINT = HSTS + 3, SECTORS = 16 };

static void send(ps1 *m, uint8_t c, const uint8_t *p, int n) {
    mem_write8(m, HSTS, 0);
    for (int i = 0; i < n; i++) mem_write8(m, DATA, p[i]);
    mem_write8(m, CMD, c);
}
static void ack(ps1 *m) { mem_write8(m, HSTS, 1); mem_write8(m, HINT, 0x1f); }
static int wait_int(ps1 *m) {
    mem_write8(m, HSTS, 1);
    // Adding 44.1 kHz SPU events exhausted the old 10,000-event guard before Stop's documented 0x18a6076-cycle response. Bound simulated time, not unrelated callbacks
    uint64_t end = m->cycles + 2u * CPU_HZ;
    while (!(mem_read8(m, HINT) & 7) && m->next_event <= end) { m->cycles = m->next_event; sched_run(m); }
    return mem_read8(m, HINT) & 7;
}
static int response(ps1 *m, uint8_t *out) { int n = 0; while (n < 16 && mem_read8(m, HSTS) & 0x20) out[n++] = mem_read8(m, CMD); return n; }
static void advance(ps1 *m, uint64_t cycles) {
    uint64_t end = m->cycles + cycles;
    while (m->next_event <= end) { m->cycles = m->next_event; sched_run(m); }
    m->cycles = end;
}
static void request(ps1 *m) { mem_write8(m, HSTS, 0); mem_write8(m, HINT, 0x80); }
static const char licence[] = "Sony Computer Entertainment Amer  ica ";   // sector 4's second line (psx-spx, CDROM ISO Volume Descriptors: System Area)
static void sector(uint8_t *s, int n) {
    for (int i = 0; i < 2352; i++) s[i] = (uint8_t)(i * 7 + n * 31);
    s[0] = s[11] = 0; memset(s + 1, 0xff, 10);
    s[12] = 0; s[13] = 2; s[14] = (uint8_t)(n / 10 * 16 + n % 10); s[15] = 2;
    s[16] = 3; s[17] = 4; s[18] = 8; s[19] = 0; memcpy(s + 20, s + 16, 4);
    if (n == 4) memcpy(s + 24 + 0x20, licence, sizeof licence - 1);
}
static int payload(ps1 *m, int n) { uint8_t s[2352]; sector(s, n); int same = 1; for (int i = 0; i < 2048; i++) if (mem_read8(m, DATA) != s[24 + i]) same = 0; return same; }
static void mode(ps1 *m, uint8_t v) { send(m, 0x0e, &v, 1); CHECK(wait_int(m) == 3, "Setmode acknowledged"); ack(m); }
static void locate(ps1 *m, uint8_t frame) {
    send(m, 0x02, (const uint8_t[]){0, 2, frame}, 3);
    CHECK(wait_int(m) == 3, "valid BCD Setloc acknowledged"); ack(m);
}
static uint64_t read_start(ps1 *m, uint8_t cmd) {
    send(m, cmd, NULL, 0); CHECK(wait_int(m) == 3, "ReadN/ReadS acknowledges before data"); ack(m);
    CHECK(wait_int(m) == 1, "ReadN/ReadS produces sector-ready INT1"); return m->cycles;
}
static void pause_read(ps1 *m) {
    uint8_t r[16]; send(m, 0x09, NULL, 0);
    CHECK(wait_int(m) == 3 && response(m, r) == 1 && r[0] == 0x22, "Pause acknowledges with old reading status 22h"); ack(m);
    CHECK(wait_int(m) == 2 && response(m, r) == 1 && r[0] == 2, "Pause completes with motor-on status 02h"); ack(m);
    advance(m, CPU_HZ / 10); mem_write8(m, HSTS, 1);
    CHECK(!(mem_read8(m, HINT) & 7), "Pause cancels subsequent sector interrupts");
}

int main(void) {
    char path[] = "/tmp/ps1native-cdread-XXXXXX.bin"; int fd = mkstemps(path, 4);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "wb"); ps1 *m = ps1_new(), *other = ps1_new();
    if (!f || !m || !other) { fputs("FAIL: cannot create synthetic disc or machines\n", stderr); if (f) fclose(f); else if (fd >= 0) close(fd); unlink(path); ps1_free(m); ps1_free(other); return 1; }
    uint8_t s[2352], r[16]; int written = 1;
    for (int n = 0; n < SECTORS; n++) { sector(s, n); if (fwrite(s, 1, sizeof s, f) != sizeof s) written = 0; }
    if (fclose(f)) written = 0;
    if (!written || disc_open(m, path) || disc_open(other, path)) { fputs("FAIL: cannot mount synthetic disc\n", stderr); unlink(path); ps1_free(m); ps1_free(other); return 1; }
    unlink(path);   // mounted descriptors stay open; no synthetic fixture is left behind
    ps1_reset(m); ps1_reset(other);
    CHECK(m->disc.ntracks == 1 && m->disc.sectors == SECTORS, "reset retains mounted disc and its TOC");
    mem_write8(m, HSTS, 1); mem_write8(m, DATA, 0x1f); m->i_mask = 1u << IRQ_CDROM;
    send(m, 0x02, (const uint8_t[]){0, 2, 0x75}, 3);
    CHECK(wait_int(m) == 5 && response(m, r) == 2 && r[1] == 0x10, "Setloc rejects frame 75 with invalid-value error"); ack(m);
    send(m, 0x02, (const uint8_t[]){0, 0x1a, 0}, 3);
    CHECK(wait_int(m) == 5 && response(m, r) == 2 && r[1] == 0x10, "Setloc rejects non-BCD seconds"); ack(m);
    send(m, 0x02, (const uint8_t[]){0, 2}, 2);
    CHECK(wait_int(m) == 5 && response(m, r) == 2 && r[1] == 0x20, "Setloc rejects wrong parameter count"); ack(m);
    send(m, 0x13, NULL, 0);
    CHECK(wait_int(m) == 3 && response(m, r) == 3 && r[1] == 1 && r[2] == 1, "GetTN reports one track in BCD"); ack(m);
    send(m, 0x14, (const uint8_t[]){1}, 1);
    CHECK(wait_int(m) == 3 && response(m, r) == 3 && r[1] == 0 && r[2] == 2, "GetTD reports INDEX01 at 00:02"); ack(m);
    send(m, 0x14, (const uint8_t[]){0}, 1);
    CHECK(wait_int(m) == 3 && response(m, r) == 3 && r[1] == 0 && r[2] == 2, "GetTD zero reports leadout, truncated to seconds"); ack(m);
    send(m, 0x14, (const uint8_t[]){0x1a}, 1);
    CHECK(wait_int(m) == 5 && response(m, r) == 2 && r[1] == 0x10, "GetTD rejects non-BCD track number"); ack(m);

    mode(m, 0); locate(m, 0); uint64_t first = read_start(m, 0x06);
    CHECK(m->i_stat & 1u << IRQ_CDROM, "sector-ready response raises IRQ2");
    CHECK(!(mem_read8(m, HSTS) & 0x40), "sector data waits for BFRD before asserting DRQSTS");
    request(m); CHECK(mem_read8(m, HSTS) & 0x40, "BFRD exposes data FIFO");
    sector(s, 0); int same = 1;
    for (int i = 0; i < 2048; i++) if (mem_read8(m, DATA) != s[24 + i]) same = 0;
    CHECK(same && !(mem_read8(m, HSTS) & 0x40), "2048-byte Mode2 payload excludes header and exhausts FIFO");
    CHECK(mem_read8(m, DATA) == s[24 + 2040], "over-read repeats documented 2048-byte padding value"); ack(m);
    CHECK(wait_int(m) == 1 && m->cycles - first == CPU_HZ / 75, "single speed delivers the next INT1 at 75 Hz");
    request(m); ack(m);
    mem_write32(m, 0x1f8010f0, m->dpcr | 1u << 15);
    mem_write32(m, 0x1f8010f4, 1u << 19 | 1u << 23);
    mem_write32(m, 0x1f8010b0, 0x10000); mem_write32(m, 0x1f8010b4, 512); mem_write32(m, 0x1f8010b8, 0x11000000);
    sector(s, 1);
    CHECK(!memcmp(m->ram + 0x10000, s + 24, 2048), "DMA3 copies the second sector into main RAM");
    CHECK(!(mem_read32(m, 0x1f8010b8) & 0x11000000) && m->dicr & 1u << 27 && m->i_stat & 1u << IRQ_DMA, "DMA3 clears busy and records completion IRQ");
    pause_read(m);
    mode(m, 0xa0); locate(m, 0); first = read_start(m, 0x1b);
    request(m); sector(s, 0);
    CHECK(mem_read16(m, DATA) == (uint16_t)(s[12] | s[13] << 8), "CPU halfword reads consume two little-endian FIFO bytes");
    same = 1; for (int i = 2; i < 2340; i++) if (mem_read8(m, DATA) != s[12 + i]) same = 0;
    CHECK(same && !(mem_read8(m, HSTS) & 0x40), "2340-byte mode includes header and excludes only sync"); ack(m);
    CHECK(wait_int(m) == 1 && m->cycles - first == CPU_HZ / 150, "double speed delivers the next INT1 at 150 Hz");
    request(m); sector(s, 1);
    advance(m, 3 * (CPU_HZ / 150)); mem_write8(m, HSTS, 1);
    CHECK((mem_read8(m, HINT) & 7) == 1, "an unacknowledged INT1 is not replaced by later responses");
    same = 1; for (int i = 0; i < 2340; i++) if (mem_read8(m, DATA) != s[12 + i]) same = 0;
    CHECK(same, "BFRD locks requested sector against subsequent incoming sectors");
    send(m, 0x10, NULL, 0); ack(m); sector(s, 4);
    CHECK(wait_int(m) == 3 && response(m, r) == 8 && !memcmp(r, s + 12, 8), "GetlocL reports newest header while data FIFO held an older sector"); ack(m);
    ps1_reset(m); advance(m, CPU_HZ / 10); mem_write8(m, HSTS, 1);
    CHECK(!(mem_read8(m, HINT) & 7) && !(mem_read8(m, HSTS) & 0x40), "reset cancels read stream and clears data FIFO");
    mode(other, 0); locate(other, 0); read_start(other, 0x06); request(other); sector(s, 0);
    CHECK(mem_read8(other, DATA) == s[24] && !(mem_read8(m, HSTS) & 0x40), "two mounted machines keep independent FIFO and stream state");

    mode(m, 0); locate(m, 0); read_start(m, 0x06); request(m); CHECK(payload(m, 0), "initial read starts at Setloc target"); ack(m);
    locate(m, 8); CHECK(wait_int(m) == 1, "Setloc during a read leaves the stream active"); request(m);
    CHECK(payload(m, 1), "Setloc stores its target without redirecting the active read"); ack(m);
    send(m, 0x11, NULL, 0);
    CHECK(wait_int(m) == 3 && response(m, r) == 8 && !memcmp(r, (const uint8_t[]){1, 1, 0, 0, 1, 0, 2, 1}, 8), "GetlocP returns BCD track, index, relative and absolute sector"); ack(m);
    send(m, 0x07, NULL, 0);
    CHECK(wait_int(m) == 5 && response(m, r) == 2 && r[0] == 0x23 && r[1] == 0x20, "MotorOn with motor already spinning reports error 20h"); ack(m);
    CHECK(wait_int(m) == 1, "MotorOn error leaves the active read running"); request(m);
    CHECK(payload(m, 2), "MotorOn error does not pause or redirect the sector stream"); ack(m);
    read_start(m, 0x06); request(m); CHECK(payload(m, 8), "the next Read consumes the new Setloc target"); ack(m); pause_read(m);
    read_start(m, 0x06); request(m); CHECK(payload(m, 8), "Read after Pause repeats the most recently received sector"); ack(m);
    locate(m, 0x15); read_start(m, 0x06); request(m); CHECK(payload(m, 15), "last sector before leadout is delivered intact"); ack(m);
    CHECK(wait_int(m) == 4 && response(m, r) == 1 && r[0] == 2, "leadout stops reading and raises INT4 with motor-on status"); ack(m);
    send(m, 0x08, NULL, 0); CHECK(wait_int(m) == 3, "Stop acknowledges before motor stops"); ack(m);
    CHECK(wait_int(m) == 2 && response(m, r) == 1 && r[0] == 0, "Stop completes with motor-off status"); ack(m);
    send(m, 0x1a, NULL, 0); CHECK(wait_int(m) == 3 && response(m, r) == 1 && r[0] == 0, "GetID acknowledges with current motor-off status"); ack(m);
    CHECK(wait_int(m) == 2 && response(m, r) == 8 && !memcmp(r, (const uint8_t[]){0, 0, 0x20, 0, 'S', 'C', 'E', 'A'}, 8), "GetID: a licensed Mode2 disc, SCEA from its licence"); ack(m);
    send(m, 0x19, (const uint8_t[]){0x22}, 1);
    CHECK(wait_int(m) == 3 && response(m, r) == 7 && !memcmp(r, "for U/C", 7), "Test 22h: the console of the licence's region"); ack(m);
    send(m, 0x19, (const uint8_t[]){0x05}, 1);
    CHECK(wait_int(m) == 3 && response(m, r) == 2 && r[0] == 1 && r[1] == 1, "Test 05h: one SCEx string read, and it matched"); ack(m);

    char bare[] = "/tmp/ps1native-unlicensed-XXXXXX.bin"; fd = mkstemps(bare, 4); f = fd < 0 ? NULL : fdopen(fd, "wb");
    int ready = f != NULL;
    for (int n = 0; ready && n < SECTORS; n++) { sector(s, n); if (n == 4) memset(s + 24, 0, 2048); ready = fwrite(s, 1, sizeof s, f) == sizeof s; }
    if (f && fclose(f)) ready = 0; else if (!f && fd >= 0) close(fd);
    if (!ready || disc_open(other, bare)) { fputs("FAIL: cannot mount synthetic unlicensed disc\n", stderr); unlink(bare); ps1_free(m); ps1_free(other); return 1; }
    unlink(bare); mode(other, 0); locate(other, 0); send(other, 0x06, NULL, 0);
    CHECK(wait_int(other) == 5 && response(other, r) == 2 && r[0] == 3 && r[1] == 0x40, "ReadN on an unlicensed disc: INT5 03h 40h, without INT3"); ack(other);
    send(other, 0x1a, NULL, 0); CHECK(wait_int(other) == 3, "GetID on an unlicensed disc: INT3 first"); ack(other);
    CHECK(wait_int(other) == 5 && response(other, r) == 8 && !memcmp(r, (const uint8_t[]){0x0a, 0x80, 0x20, 0, 0, 0, 0, 0}, 8), "GetID: unlicensed Mode2, no SCEx"); ack(other);
    mode(other, 1); locate(other, 0); read_start(other, 0x06); request(other);
    CHECK(payload(other, 0), "with CDDA mode on, the unlicensed disc reads"); ack(other);

    char mode1[] = "/tmp/ps1native-mode1-XXXXXX.bin"; fd = mkstemps(mode1, 4); f = fd < 0 ? NULL : fdopen(fd, "wb");
    ready = f != NULL;
    for (int n = 0; ready && n < 5; n++) {
        sector(s, n); s[15] = 1;
        if (n == 4) { memset(s + 16, 0, 2048); memcpy(s + 16 + 0x20, licence, sizeof licence - 1); }
        ready = fwrite(s, 1, sizeof s, f) == sizeof s;
    }
    if (f && fclose(f)) ready = 0; else if (!f && fd >= 0) close(fd);
    if (!ready || disc_open(m, mode1)) { fputs("FAIL: cannot mount synthetic Mode1 disc\n", stderr); unlink(mode1); ps1_free(m); ps1_free(other); return 1; }
    unlink(mode1); ps1_reset(m); mode(m, 0); locate(m, 0); read_start(m, 0x06); request(m); sector(s, 0);
    same = 1; for (int i = 0; i < 2048; i++) if (mem_read8(m, DATA) != s[16 + i]) same = 0;
    CHECK(same && !(mem_read8(m, HSTS) & 0x40), "Mode1 2048-byte payload begins after its 16-byte sync/header");
    disc_close(other);
    CHECK(!other->disc.ntracks && !(mem_read8(other, HSTS) & 0x60) && !mem_read8(other, DATA), "closing a live disc clears its data and response FIFOs");
    advance(other, CPU_HZ / 5); mem_write8(other, HSTS, 1);
    CHECK(!(mem_read8(other, HINT) & 7), "closing a live disc cancels pending CD responses and sector events");
    send(other, 0x06, NULL, 0);
    CHECK(wait_int(other) == 5 && response(other, r) == 2 && r[1] == 0x80, "reads after close report the empty drive");
    ps1_free(m); ps1_free(other);
    printf("cdread: %d checks, %d failed\n", checks, fails); return fails != 0;
}
