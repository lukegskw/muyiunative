// Multi-disc controller protocol with synthetic licensed sectors only. psx-spx, CDROM Status Commands (Status code/Nop/GetID): unsolicited door-open INT5, sticky
// ShellOpen until Nop after closure, and re-identification of the next disc. Mainloop/Responses requires ACK before replacing an already delivered response.
#include "ps1.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void send(ps1 *m, uint8_t c, const uint8_t *p, int n) {
    cd_write(m, 0, 0); for (int i = 0; i < n; i++) cd_write(m, 2, p[i]); cd_write(m, 1, c);
}
static int wait_int(ps1 *m) {
    for (int i = 0; i < 10000 && !(m->cd.hintsts & 7); i++) { m->cycles = m->next_event; sched_run(m); }
    return m->cd.hintsts & 7;
}
static int response(ps1 *m, uint8_t *out) { int n = 0; while (n < 16 && cd_read(m, 0) & 0x20) out[n++] = cd_read(m, 1); return n; }
static void ack(ps1 *m) { cd_write(m, 0, 1); cd_write(m, 3, 0x1f); }
static int image(const char *path, const char *region, uint8_t marker) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    for (int i = 0; i < 10; i++) {
        uint8_t s[DISC_SECTOR] = {0}; memset(s + 1, 0xff, 10); s[15] = 2; s[24] = marker;
        if (i == 4) snprintf((char *)s + 24 + 0x20, 80, "Sony Computer Entertainment %s", region);
        if (fwrite(s, 1, sizeof s, f) != sizeof s) { fclose(f); return -1; }
    }
    return fclose(f);
}
int main(void) {
    char dir[] = "/tmp/muyiunative-cdswap-XXXXXX", a[128], b[128]; uint8_t r[16]; ps1 *m = ps1_new(), *other = ps1_new();
    if (!m || !other || !mkdtemp(dir)) return 1;
    snprintf(a, sizeof a, "%s/a.bin", dir); snprintf(b, sizeof b, "%s/b.bin", dir);
    if (image(a, "America", 0x31) || image(b, "Europe", 0x72)) return 1;
    CHECK(!disc_open(m, a) && !disc_open(other, a) && m->cd.region == 'A', "first synthetic disc is licensed");
    cd_write(m, 0, 1); cd_write(m, 2, 0x1f);
    m->cd.mode = 0x80; m->cd.atv[0] = 0x63; m->cd.filter_file = 7;
    send(m, 6, NULL, 0); CHECK(wait_int(m) == 3 && m->cd.reading, "ReadN begins before opening the cover"); ack(m);
    CHECK(wait_int(m) == 1, "first disc delivers a sector");
    cd_write(m, 0, 0); cd_write(m, 3, 0x80); m->cd.audio_n = 3; m->cd.audio[0] = 123;
    cd_set_shell(m, 1);
    CHECK(m->cd.shell_open && m->cd.stat == 0x10 && !m->cd.reading && !m->cd.playing && m->cd.sector_next == NEVER,
          "opening stops the drive and records the physical switch separately from status");
    CHECK(!m->cd.audio_n && !m->cd.data_n && !m->cd.have_sector && !m->cd.have_header, "old disc audio, header and sector FIFOs cannot leak into the next disc");
    CHECK((m->cd.hintsts & 7) == 1 && response(m, r) == 1 && (r[0] & 0x20), "a delivered sector response remains intact until acknowledged");
    send(m, 1, NULL, 0); ack(m);
    CHECK(wait_int(m) == 5 && response(m, r) == 2 && r[0] == 0x11 && r[1] == 8 && (m->i_stat & (1u << IRQ_CDROM)),
          "door INT5 reports ShellOpen/error 08h and raises the enabled host IRQ before a queued command"); ack(m);
    CHECK(wait_int(m) == 3 && response(m, r) == 1 && r[0] == 0x10, "queued Nop observes the open cover"); ack(m);
    for (int i = 0; i < 2; i++) {
        send(m, 1, NULL, 0); CHECK(wait_int(m) == 3 && response(m, r) == 1 && r[0] == 0x10, "Nop never clears ShellOpen while the cover remains open"); ack(m);
    }
    send(m, 0x1a, NULL, 0); CHECK(wait_int(m) == 5 && response(m, r) == 2 && r[0] == 0x11 && r[1] == 0x80, "GetID cannot identify a disc with the cover open"); ack(m);
    send(m, 0x13, NULL, 0); CHECK(wait_int(m) == 5 && response(m, r) == 2 && r[1] == 0x80, "TOC queries cannot see through the open cover"); ack(m);
    CHECK(!disc_open(m, b) && m->cd.shell_open && m->cd.hintmsk == 0x1f && m->cd.mode == 0x80 && m->cd.atv[0] == 0x63 && m->cd.filter_file == 7,
          "inserting the next disc keeps controller IRQ mask, mode, filter and audio matrix");
    cd_set_shell(m, 0);
    CHECK(m->cd.region == 'E' && m->cd.scex == 1 && !m->cd.shell_open, "closure identifies the next disc's licence");
    send(m, 1, NULL, 0); CHECK(wait_int(m) == 3 && response(m, r) == 1 && r[0] == 0x12 && m->cd.stat == 2, "first closed Nop returns then clears the sticky ShellOpen bit"); ack(m);
    send(m, 1, NULL, 0); CHECK(wait_int(m) == 3 && response(m, r) == 1 && r[0] == 2, "next Nop observes closed, spinning media"); ack(m);
    send(m, 0x1a, NULL, 0); CHECK(wait_int(m) == 3, "new disc GetID first response"); ack(m);
    CHECK(wait_int(m) == 2 && response(m, r) == 8 && r[7] == 'E', "GetID returns the new disc's identity"); ack(m);
    send(m, 2, (const uint8_t[]){0, 2, 0}, 3); CHECK(wait_int(m) == 3, "Setloc on the next disc"); ack(m);
    send(m, 6, NULL, 0); CHECK(wait_int(m) == 3, "ReadN on the next disc"); ack(m);
    CHECK(wait_int(m) == 1, "new disc delivers data through the controller"); cd_write(m, 0, 0); cd_write(m, 3, 0x80);
    CHECK(cd_read(m, 2) == 0x72 && other->cd.region == 'A' && !other->cd.shell_open, "controller reads next disc bytes while another console retains its disc and cover"); ack(m);

    // Open during a two-response command: the removed disc's delayed completion must not overwrite the new cover state.
    send(m, 9, NULL, 0); CHECK(wait_int(m) == 3, "Pause's first response arrives"); ack(m); cd_set_shell(m, 1);
    CHECK(wait_int(m) == 5 && response(m, r) == 2 && r[1] == 8 && m->cd.complete_stat == -1, "opening cancels a pending Pause completion"); ack(m);
    cd_set_shell(m, 1); CHECK(m->cd.next == NEVER && !m->cd.shell_pending, "opening an already open cover does not duplicate the notification");
    ps1_reset(m); CHECK(m->cd.shell_open && m->cd.stat == 0x10 && !m->cd.region, "console reset cannot physically close the cover");
    cd_set_shell(m, 0); send(m, 1, NULL, 0); cd_set_shell(m, 1);
    CHECK(wait_int(m) == 3, "an in-flight command's first response stays ordered"); ack(m);
    CHECK(wait_int(m) == 5 && response(m, r) == 2 && r[1] == 8, "door event follows the in-flight first response"); ack(m);
    ps1_free(m); ps1_free(other); unlink(a); unlink(b); rmdir(dir);
    printf("cdswap: %d checks, %d failed\n", checks, fails); return fails != 0;
}
