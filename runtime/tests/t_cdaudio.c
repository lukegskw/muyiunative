// CD audio and the commands around it (cdrom.c, spu.c), on a disc built here in /tmp: a licensed data track with XA-ADPCM and data sectors interleaved on two
// channels, and an audio track. psx-spx, CDROM Drive: Data/ADPCM Sector Filtering/Delivery, Setmode, Setfilter, Play, Report, AutoPause, Mute, ATV0-3 and ADPCTL,
// SetSession, GetQ, the Command Summary's parameter counts; CDROM Format: XA-ADPCM and its 25-point zigzag interpolation; SPU: ATTR, AVOLL/AVOLR, capture buffers.
// No game files
#include "ps1.h"
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
enum { HSTS = 0x1f801800, CMD = HSTS + 1, DATA = HSTS + 2, HINT = HSTS + 3, DATA_END = 20, SECTORS = 40, L = 1000, R = -2000 };

static ps1 *m;
static void send(uint8_t c, const uint8_t *p, int n) {
    mem_write8(m, HSTS, 0);
    for (int i = 0; i < n; i++) mem_write8(m, DATA, p[i]);
    mem_write8(m, CMD, c);
}
static void ack(void) { mem_write8(m, HSTS, 1); mem_write8(m, HINT, 0x1f); }
static int wait_int(void) {
    mem_write8(m, HSTS, 1);
    uint64_t end = m->cycles + 8u * CPU_HZ;
    while (!(mem_read8(m, HINT) & 7) && m->next_event <= end) { m->cycles = m->next_event; sched_run(m); }
    return mem_read8(m, HINT) & 7;
}
static int response(uint8_t *out) { int n = 0; while (n < 16 && mem_read8(m, HSTS) & 0x20) out[n++] = mem_read8(m, CMD); return n; }
static int simple(uint8_t c, const uint8_t *p, int n) { send(c, p, n); int t = wait_int(); ack(); return t; }
static void port(int bank, int off, uint8_t v) { mem_write8(m, HSTS, (uint8_t)bank); mem_write8(m, HSTS + (uint32_t)off, v); }
static int16_t frame(unsigned i, int ch) { return m->cd.audio[(m->cd.audio_read + i) % CD_AUDIO_FRAMES * 2 + (unsigned)ch]; }   // i-th buffered frame

static void sector(uint8_t *s, int n) {
    memset(s, 0, 2352);
    if (n >= DATA_END) { for (int i = 0; i < 588; i++) { s[i * 4] = (uint8_t)L; s[i * 4 + 1] = (uint8_t)(L >> 8); s[i * 4 + 2] = (uint8_t)R; s[i * 4 + 3] = (uint8_t)(R >> 8); } return; }
    memset(s + 1, 0xff, 10); s[13] = 2; s[14] = (uint8_t)(n / 10 * 16 + n % 10); s[15] = 2;
    int xa = n == 16 || n == 18;   // XA-ADPCM on file 1, channels 0 and 1, data between them on the other channel
    s[16] = n >= 16; s[17] = n == 17 || n == 18; s[18] = xa ? 0x64 : 0x08; memcpy(s + 20, s + 16, 4);
    for (int i = 24; i < 2072; i++) s[i] = (uint8_t)(i + n * 13);
    if (n == 4) memcpy(s + 24 + 0x20, "Sony Computer Entertainment Amer  ica ", 38);
    if (xa) { memset(s + 24, 0, 18 * 128); s[24 + 16] = 0x08; }   // mono 37.8 kHz 4-bit, every shift and filter 0: one sample of -8000h, then silence
}

int main(void) {
    char bin[] = "/tmp/ps1native-cdaudio-XXXXXX.bin", cue[sizeof bin + 4]; int fd = mkstemps(bin, 4); FILE *f = fd < 0 ? NULL : fdopen(fd, "wb");
    uint8_t s[2352], r[16]; int ready = f != NULL;
    for (int n = 0; ready && n < SECTORS; n++) { sector(s, n); ready = fwrite(s, 1, sizeof s, f) == sizeof s; }
    if (f && fclose(f)) ready = 0; else if (!f && fd >= 0) close(fd);
    snprintf(cue, sizeof cue, "%s.cue", bin); f = ready ? fopen(cue, "w") : NULL;
    ready = f && fprintf(f, "FILE \"%s\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:00:%02d\n", bin, DATA_END) > 0;
    if (f && fclose(f)) ready = 0;
    m = ps1_new();
    if (!ready || !m || disc_open(m, cue)) { fputs("FAIL: cannot build or mount the synthetic disc\n", stderr); unlink(bin); unlink(cue); ps1_free(m); return 1; }
    unlink(bin); unlink(cue);
    mem_write8(m, HSTS, 1); mem_write8(m, DATA, 0x1f);

    // Parameter counts and invalid commands (Command Summary)
    CHECK(simple(0x1c, (const uint8_t[]){1, 2, 3}, 3) == 3, "Reset takes any number of parameters");
    send(0x00, (const uint8_t[]){1}, 1); CHECK(wait_int() == 5 && response(r) == 2 && r[1] == 0x40, "an invalid command with a parameter: error 40h, not 20h"); ack();
    send(0x12, (const uint8_t[]){0}, 1); CHECK(wait_int() == 5 && response(r) == 2 && r[1] == 0x10, "SetSession 0: error 10h"); ack();
    CHECK(simple(0x12, (const uint8_t[]){1}, 1) == 3 && wait_int() == 2, "SetSession 1: INT3, then INT2"); ack();
    CHECK(simple(0x12, (const uint8_t[]){2}, 1) == 3 && wait_int() == 5 && response(r) == 2 && r[0] == 6 && r[1] == 0x40, "SetSession past the last session: INT5 06h 40h"); ack();
    send(0x04, NULL, 0); CHECK(wait_int() == 5 && response(r) == 2 && r[1] == 0x80, "Forward while not playing: error 80h"); ack();
    CHECK(simple(0x1d, (const uint8_t[]){1, 0x02}, 2) == 3 && wait_int() == 2 && response(r) == 11 &&
          !memcmp(r, (const uint8_t[]){0x01, 0, 0x02, 0, 0, 0, 0, 0x00, 0x02, 0x20, 0}, 11), "GetQ ADR 1 point 02h: an audio track starting at 00:02:20"); ack();
    CHECK(simple(0x1d, (const uint8_t[]){1, 0xa0}, 2) == 3 && wait_int() == 2 && response(r) == 11 && r[0] == 0x41 && r[7] == 1 && r[8] == 0x20,
          "GetQ point A0h: first track 1 of a CD-ROM XA disc"); ack();
    CHECK(simple(0x1d, (const uint8_t[]){1, 0xa2}, 2) == 3 && wait_int() == 2 && response(r) == 11 && r[7] == 0 && r[8] == 2 && r[9] == 0x40,
          "GetQ point A2h: the lead-out at 00:02:40"); ack();

    // XA-ADPCM with the filter: sector 16 (file 1, channel 0) is decoded, 17 (data, channel 1) reaches the CPU at its first attempt, 18 (XA, channel 1) neither
    m->spu.next = NEVER; sched_update(m);   // keep the decoded frames for this test
    CHECK(simple(0x0d, (const uint8_t[]){1, 0}, 2) == 3 && simple(0x0e, (const uint8_t[]){0x48}, 1) == 3 && simple(0x02, (const uint8_t[]){0, 2, 0x16}, 3) == 3,
          "Setfilter, Setmode XA and filter, Setloc");
    CHECK(simple(0x1b, NULL, 0) == 3, "ReadS");
    CHECK(wait_int() == 1, "an INT1"); send(0x10, NULL, 0); ack();
    CHECK(wait_int() == 3 && response(r) == 8 && r[2] == 0x17 && r[5] == 1, "the first INT1 is sector 17, a data sector on the other channel"); ack();
    CHECK(wait_int() == 1, "the next INT1"); send(0x10, NULL, 0); ack();
    CHECK(wait_int() == 3 && response(r) == 8 && r[2] == 0x19 && r[5] == 0, "then sector 19: XA sector 18 on channel 1 is neither played nor delivered"); ack();
    CHECK(simple(0x09, NULL, 0) == 3 && wait_int() == 2, "Pause"); ack();
    CHECK(m->cd.audio_n == 4704 && mem_read8(m, HSTS) & 4, "one mono 37.8 kHz sector makes 4704 frames at 44.1 kHz, and ADPBUSY shows them");
    static const int16_t impulse[14] = {2, -3, 5, -5, -10, -26, 68, -9, -306, 926, -2488, 6016, -21472, -21472};
    int same = 1; int16_t o[2];
    for (int i = 0; i < 14; i++) { cd_audio(m, o); if (o[0] != impulse[i] || o[1] != impulse[i]) same = 0; }
    CHECK(same, "an impulse of -8000h comes out as the zigzag tables' taps 6 and 12, negated, on both channels");
    port(1, 3, 0x20); CHECK(!m->cd.audio_n && !(mem_read8(m, HSTS) & 4), "HCLRCTL.5 clears the XA-ADPCM buffer");
    CHECK(simple(0x0e, (const uint8_t[]){0}, 1) == 3 && simple(0x02, (const uint8_t[]){0, 2, 0x16}, 3) == 3 && simple(0x1b, NULL, 0) == 3, "XA off: ReadS");
    CHECK(wait_int() == 1, "XA off: an INT1"); send(0x10, NULL, 0); ack();
    CHECK(wait_int() == 3 && response(r) == 8 && r[2] == 0x16, "XA off: the XA sector reaches the CPU as data"); ack();
    CHECK(simple(0x09, NULL, 0) == 3 && wait_int() == 2, "Pause"); ack();

    // Play into the audio track: AutoPause at the track change, and again on resuming there; Report every ten sectors; Mute, ADPMUTE and the ATV matrix
    CHECK(simple(0x0e, (const uint8_t[]){0x06}, 1) == 3 && simple(0x02, (const uint8_t[]){0, 2, 0x18}, 3) == 3, "Setmode AutoPause and Report, Setloc 00:02:18");
    CHECK(simple(0x03, NULL, 0) == 3 && wait_int() == 4 && response(r) == 1 && r[0] == 2, "Play from the data track pauses at the audio track: INT4, motor on"); ack();
    CHECK(m->cd.audio_n == 1176 && !frame(0, 0) && !frame(1175, 1), "the two data sectors played as silence");
    CHECK(simple(0x03, NULL, 0) == 3 && wait_int() == 4, "Play where it paused pauses again"); ack();
    m->cd.audio_n = 0;
    CHECK(simple(0x03, (const uint8_t[]){0x02}, 1) == 3, "Play track 2");
    CHECK(wait_int() == 1 && response(r) == 8 && !memcmp(r, (const uint8_t[]){0x82, 0x02, 0x01, 0x00, 0x02, 0x20, L & 255, L >> 8}, 8),
          "Report at absolute frame 20: playing, track 2, index 1, 00:02:20, the left peak"); ack();
    CHECK(wait_int() == 1 && response(r) == 8 && !memcmp(r, (const uint8_t[]){0x82, 0x02, 0x01, 0x00, 0x80, 0x10}, 6), "Report at frame 30: the time in the track, seconds +80h");
    ack();
    cd_audio(m, o); CHECK(o[0] == L && o[1] == R, "CD-DA frames pass at unity ATV");
    port(2, 2, 0x40); port(2, 3, 0x40); port(3, 1, 0x40); port(3, 2, 0x40);
    cd_audio(m, o); CHECK(o[0] == L && o[1] == R, "written ATV values wait for ADPCTL.5");
    port(3, 3, 0x21); cd_audio(m, o); CHECK(o[0] == (L + R) / 2 && o[1] == (L + R) / 2, "ATV 40h all round mixes to mono; ADPMUTE leaves CD-DA alone");
    port(3, 3, 0); CHECK(simple(0x0b, NULL, 0) == 3, "Mute"); cd_audio(m, o); CHECK(!o[0] && !o[1], "Mute silences CD audio");
    CHECK(simple(0x0c, NULL, 0) == 3, "Demute"); cd_audio(m, o); CHECK(o[0] == (L + R) / 2, "Demute restores it");
    CHECK(wait_int() == 4 && response(r) == 1 && r[0] == 2, "AutoPause at the lead-out: INT4, motor on"); ack();
    port(2, 2, 0x80); port(2, 3, 0); port(3, 1, 0x80); port(3, 2, 0); port(3, 3, 0x20);

    // Into the SPU: AVOLL/AVOLR, then the main volume; neither SPU enable nor mute applies; the capture buffers take the frames before the SPU's volume
    ps1_reset(m); mem_write8(m, HSTS, 1); mem_write8(m, DATA, 0x1f);
    mem_write16(m, 0x1f801daa, 0x0001); mem_write16(m, 0x1f801dac, 4); mem_write16(m, 0x1f801db0, 0x4000); mem_write16(m, 0x1f801db2, 0x4000);
    mem_write16(m, 0x1f801d80, 0x3fff); mem_write16(m, 0x1f801d82, 0x3fff);
    CHECK(simple(0x03, (const uint8_t[]){0x02}, 1) == 3, "Play track 2, SPU off and muted, CD input on");
    while (m->cd.stat != 0x82 || m->cd.audio_n > 100) { m->cycles = m->next_event; sched_run(m); }
    static int16_t out[SPU_AUDIO_FRAMES * 2]; int n = spu_audio_read(m, out, SPU_AUDIO_FRAMES), heard = 0;
    for (int i = n - 64; i < n; i++) heard += i >= 0 && out[i * 2] == 499 && out[i * 2 + 1] == -1000;
    CHECK(heard == 64, "CD audio at AVOL 4000h and main volume 3FFFh: (499, -1000)");
    unsigned at = (m->spu.capture - 2) & 0x3ff;
    CHECK((int16_t)(m->spu.ram[at] | m->spu.ram[at + 1] << 8) == L && (int16_t)(m->spu.ram[0x400 + at] | m->spu.ram[0x401 + at] << 8) == R,
          "the capture buffers hold the CD's frames before the SPU's volume");
    mem_write16(m, 0x1f801daa, 0); for (int i = 0; i < 64; i++) { m->cycles = m->spu.next; sched_run(m); }
    n = spu_audio_read(m, out, SPU_AUDIO_FRAMES); heard = 0;
    for (int i = n - 32; i < n; i++) heard += i < 0 || out[i * 2] || out[i * 2 + 1];
    CHECK(!heard, "with ATTR.0 off the CD is not heard");

    ps1_free(m);
    printf("cdaudio: %d checks, %d failed\n", checks, fails); return fails != 0;
}
