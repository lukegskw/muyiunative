// External-disc addressing (plan.md, phase 2). Inputs are synthetic raw sectors written under /tmp, never game data. psx-spx, CDROM File Formats, CUE/BIN/CDT: shared
// and separate BINs, INDEX00 stored pregap, INDEX01 disc origin, relative names, and a lead-out calculated from file sizes; unsupported layouts fail atomically
#include "ps1.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static int bin(const char *path, int sectors, uint8_t marker) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    for (int i = 0; i < sectors; i++) {
        uint8_t b[DISC_SECTOR] = {0}; memset(b + 1, 0xff, 10); b[15] = 2; b[100] = (uint8_t)(marker + i);
        if (fwrite(b, 1, sizeof b, f) != sizeof b) { fclose(f); return -1; }
    }
    return fclose(f);
}
static int cue(const char *path, const char *text) { FILE *f = fopen(path, "w"); if (!f) return -1; int bad = fputs(text, f) == EOF; return fclose(f) || bad ? -1 : 0; }

int main(void) {
    char dir[] = "/tmp/ps1native-disc-XXXXXX", a[128], b[128], c[128], partial[128]; uint8_t raw[DISC_SECTOR];
    ps1 *m = ps1_new(), *other = ps1_new();
    if (!m || !other || !mkdtemp(dir)) { fputs("FAIL: cannot set up synthetic disc test\n", stderr); return 1; }
    snprintf(a, sizeof a, "%s/first track.bin", dir); snprintf(b, sizeof b, "%s/second.bin", dir);
    snprintf(c, sizeof c, "%s/disc.cue", dir); snprintf(partial, sizeof partial, "%s/partial.bin", dir);
    if (bin(a, 8, 0x10) || bin(b, 3, 0x40)) { fputs("FAIL: cannot write synthetic sectors\n", stderr); return 1; }

    CHECK(!disc_open(m, a) && m->disc.ntracks == 1 && m->disc.sectors == 8 && m->disc.track[0].mode == 2, "bare raw BIN: size and mode from sector header");
    CHECK(!disc_read(m, 0, raw) && raw[100] == 0x10, "raw BIN's first sector");
    CHECK(!disc_read(m, 7, raw) && raw[100] == 0x17, "raw BIN's last sector");
    CHECK(disc_read(m, 8, raw) == -1 && disc_read(m, UINT32_MAX, raw) == -1 && disc_track(m, 8) == -1, "lead-out and overflowing addresses are outside the disc");
    CHECK(!disc_open(other, b) && !disc_read(other, 2, raw) && raw[100] == 0x42 && !disc_read(m, 1, raw) && raw[100] == 0x11, "two machines keep independent streams and positions");
    ps1_reset(m);
    CHECK(m->disc.sectors == 8 && !disc_read(m, 2, raw) && raw[100] == 0x12, "reset preserves mounted media");

    CHECK(!cue(c, "REM synthetic\nFILE \"first track.bin\" BINARY\r\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00 ; comment\n TRACK 02 AUDIO\n INDEX 00 00:00:02\n INDEX 01 00:00:03\n TRACK 03 AUDIO\n INDEX 01 00:00:05\n") && !disc_open(m, c), "shared BIN: quoted relative filename, CRLF, comments, three tracks");
    CHECK(m->disc.ntracks == 3 && m->disc.sectors == 8 && m->disc.track[1].start == 3 && m->disc.track[1].begin == 2 && m->disc.track[1].audio, "TOC distinguishes INDEX00 from INDEX01");
    CHECK(disc_track(m, 1) == 0 && disc_track(m, 2) == 1 && disc_track(m, 4) == 1 && disc_track(m, 5) == 2, "stored pregap belongs to the following track");
    CHECK(!disc_read(m, 2, raw) && raw[100] == 0x12 && !disc_read(m, 5, raw) && raw[100] == 0x15, "track boundaries do not shift shared BIN sector offsets");

    CHECK(!cue(c, "FILE \"first track.bin\" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\nFILE second.bin BINARY\nTRACK 02 AUDIO\nINDEX 00 00:00:00\nINDEX 01 00:00:01\n") && !disc_open(m, c), "multiple BINs mount with file-relative index times");
    CHECK(m->disc.sectors == 11 && m->disc.track[1].begin == 8 && m->disc.track[1].start == 9 && m->disc.track[0].mode == 1, "multi-file TOC accumulates the complete previous file");
    CHECK(!disc_read(m, 7, raw) && raw[100] == 0x17 && !disc_read(m, 8, raw) && raw[100] == 0x40 && !disc_read(m, 10, raw) && raw[100] == 0x42, "reads cross from shared disc addresses into the correct file offsets");

    CHECK(!cue(c, "FILE \"first track.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 00 00:00:00\nINDEX 01 00:00:01\nTRACK 02 AUDIO\nINDEX 00 00:00:03\nINDEX 01 00:00:04\n") && !disc_open(m, c), "first track may include stored lead-in pregap");
    CHECK(m->disc.sectors == 7 && m->disc.track[0].start == 0 && m->disc.track[1].start == 3 && !disc_read(m, 0, raw) && raw[100] == 0x11, "INDEX01 of first track is LBA0, including when first BIN stores pregap");

    static const char *bad[] = {
        "", "TRACK 01 MODE2/2352\nINDEX 01 00:00:00\n", "FILE absent.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n",
        "FILE second.bin BINARY\nTRACK 02 AUDIO\nINDEX 01 00:00:00\n", "FILE second.bin BINARY\nTRACK 01 AUDIO\n",
        "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:03\n", "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:60:00\n",
        "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:75\n", "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nINDEX 01 00:00:01\n",
        "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 00:00:00\n",
        "FILE second.bin BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n", "FILE second.bin BINARY\nTRACK 01 AUDIO\nPREGAP 00:02:00\nINDEX 01 00:00:00\n",
        "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nPOSTGAP 00:02:00\n", "FILE second.bin BINARY\nFILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n",
        "FILE second.bin WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n", "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nFILE second.bin BINARY\n",
        "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 00 00:00:02\nINDEX 01 00:00:01\n", "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00 trailing\n", "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n\"unterminated\n", "FILE second.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00 \"unterminated\n"
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        m->cd.mode = 0x44;
        CHECK(!cue(c, bad[i]) && disc_open(m, c) == -1 && m->disc.sectors == 7 && m->cd.mode == 0x44 && !disc_read(m, 0, raw) && raw[100] == 0x11, "malformed/unsupported CUE preserves the existing mount and drive state");
    }
    CHECK(!cue(partial, "short") && disc_open(m, partial) == -1 && m->disc.sectors == 7, "partial raw sector is rejected atomically");
    CHECK(!truncate(a, DISC_SECTOR) && disc_read(m, 1, raw) == -1, "host truncation produces a read error instead of stale sector bytes");
    disc_close(m); disc_close(m);
    CHECK(!m->disc.ntracks && disc_read(m, 0, raw) == -1 && !m->cd.stat && m->cd.sector_next == NEVER, "ejection closes streams, resets the drive and is safe twice");
    ps1_free(m); ps1_free(other); unlink(a); unlink(b); unlink(c); unlink(partial); rmdir(dir);
    printf("disc: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
