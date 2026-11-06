// Loading a later-disc quick state from the default app launch must select its bundled media, with no guest-visible swap on a failed load. Synthetic discs only.
#include "native.h"
#include "../state.h"
#include "../product.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static const ps1_native_set native = {.name = "synthetic"};
static void sink(void *ctx, const char *line) { (void)ctx; (void)line; }
static int image(const char *path, int marker) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    for (int i = 0; i < 40; i++) {
        uint8_t sector[DISC_SECTOR] = {0}; memset(sector + 1, 0xff, 10); sector[15] = 2;
        if (i == 39) sector[2000] = (uint8_t)marker;
        if (fwrite(sector, 1, sizeof sector, f) != sizeof sector) { fclose(f); return -1; }
    }
    return fclose(f);
}
int main(void) {
    char temp[] = "/tmp/muyiunative-state-discs-XXXXXX", error[FRONT_STATE_ERROR], path[256];
    char *root = mkdtemp(temp); ps1 *m = ps1_new(), *before = malloc(sizeof *before); front_product *p = calloc(1, sizeof *p);
    if (!root || !m || !before || !p) return 1;
    int directory = open(root, O_RDONLY | O_DIRECTORY); p->ndiscs = 4;
    for (int i = 0; i < p->ndiscs; i++) {
        snprintf(p->discs[i], sizeof p->discs[i], "%s/disc%d.bin", root, i + 1);
        CHECK(!image(p->discs[i], i + 1), "four equal-layout discs differ only in later data");
    }
    CHECK(!disc_open(m, p->discs[3]), "later disc mounts for the saved state");
    m->ram[100] = 0x44; m->cpu.r[9] = 1234; m->cd.mode = 0xe0; m->cd.filter_file = 7; m->cycles = 6789; sched_update(m);
    uint8_t saved_cd[sizeof m->cd]; memcpy(saved_cd, &m->cd, sizeof saved_cd);
    CHECK(!front_state_save(directory, "disc-set", m, 456, 0, NULL, error), "an existing-format state saves on Disc 4");
    CHECK(!disc_open(m, p->discs[0]), "a new app launch starts with Disc 1");
    m->ram[100] = 0x11; m->cpu.r[9] = 9876; sio_card_format(m, 0); m->sio.card[0].data[1000] = 0x99; m->sio.card[0].dirty = 1;
    m->tty_out = sink; m->tty_ctx = m; ps1_native_add(m, &native);
    uint64_t identity; disc_identity(m, &identity); memcpy(before, m, sizeof *m);
    front_snapshot *s = front_state_read_bundled(directory, "disc-set", m, p, 0, error);
    CHECK(s && s->disc_index == 3 && s->frame == 456, "loading from Disc 1 finds Disc 4 by its full content identity");
    CHECK(!memcmp(before, m, sizeof *m), "validating a later-disc state leaves every running console field and stream attached");
    front_state_free(s);
    CHECK(!memcmp(before, m, sizeof *m), "discarding a prepared load leaves the original machine unchanged");
    p->ndiscs = 3;
    CHECK(!front_state_read_bundled(directory, "disc-set", m, p, 0, error) && strstr(error, "not available"), "a disc absent from the app gives an actionable error");
    CHECK(!memcmp(before, m, sizeof *m), "an unavailable saved disc preserves the running game and its media");
    p->ndiscs = 4;
    CHECK(!front_state_read_bundled(directory, "other-game", m, p, 0, error), "automatic selection still rejects another game");
    m->bios[0] = 1;
    CHECK(!front_state_read_bundled(directory, "disc-set", m, p, 0, error), "automatic selection still rejects another BIOS"); m->bios[0] = 0;
    snprintf(path, sizeof path, "%s/quick.ps1state", root); int fd = open(path, O_RDWR); uint8_t byte;
    pread(fd, &byte, 1, 300); byte ^= 1; pwrite(fd, &byte, 1, 300); close(fd);
    CHECK(!front_state_read_bundled(directory, "disc-set", m, p, 0, error) && strstr(error, "damaged"), "a damaged later-disc state is rejected before mounting anything");
    CHECK(!memcmp(before, m, sizeof *m), "invalid payload, game and BIOS failures retain the live console");
    fd = open(path, O_RDWR); byte ^= 1; pwrite(fd, &byte, 1, 300); close(fd);
    char original[PATH_MAX]; snprintf(original, sizeof original, "%s", p->discs[1]); snprintf(p->discs[1], sizeof p->discs[1], "%s/missing.bin", root);
    s = front_state_read_bundled(directory, "disc-set", m, p, 0, error);
    CHECK(s && s->disc_index == 3, "an unavailable unrelated catalog entry does not hide the matching disc");
    if (s) {
        front_state_apply(m, s);
        uint8_t sector[DISC_SECTOR];
        CHECK(!disc_read(m, 39, sector) && sector[2000] == 4, "applying the state attaches the saved disc's actual data");
        CHECK(m->ram[100] == 0x44 && m->cpu.r[9] == 1234 && m->cycles == 6789 && !memcmp(&m->cd, &saved_cd, sizeof saved_cd),
              "console restoration retains the saved CD controller without resetting it or simulating a cover event");
        CHECK(m->tty_ctx == m && m->tty_out == sink && m->native[0] == &native && m->sio.card[0].data[1000] == 0x99 && m->sio.card[0].dirty,
              "automatic media selection preserves callbacks, native code and current memory-card contents");
    }
    front_state_free(s); snprintf(p->discs[1], sizeof p->discs[1], "%s", original);
    s = front_state_read_bundled(directory, "disc-set", m, p, 3, error);
    CHECK(s && s->disc_index == 3 && !s->media, "already mounted matching media needs no replacement"); front_state_free(s);
    CHECK(!disc_open(m, p->discs[0]) && !front_state_read(directory, "disc-set", m, error), "external-disc hosts still require the saved disc explicitly");
    CHECK(!image(p->discs[3], 5), "a same-layout different revision replaces the bundled candidate");
    disc_identity(m, &identity); memcpy(before, m, sizeof *m);
    CHECK(!front_state_read_bundled(directory, "disc-set", m, p, 0, error) && !memcmp(before, m, sizeof *m),
          "a different bundled revision never substitutes for the exact saved media");
    CHECK(!front_state_save(directory, "disc-set", m, 999, 0, NULL, error) && !disc_open(m, p->discs[2]),
          "an earlier-disc state can be loaded while playing a later disc");
    s = front_state_read_bundled(directory, "disc-set", m, p, 2, error);
    CHECK(s && s->disc_index == 0 && s->frame == 999, "automatic selection also finds the earlier disc");
    if (s) {
        front_state_apply(m, s); uint8_t sector[DISC_SECTOR];
        CHECK(!disc_read(m, 39, sector) && sector[2000] == 1, "rewinding to an earlier disc attaches its matching streams");
    }
    front_state_free(s);
    for (int i = 0; i < p->ndiscs; i++) unlink(p->discs[i]);
    unlink(path); close(directory); CHECK(!rmdir(root), "state lookup leaves no temporary files");
    ps1_free(m); free(before); free(p); printf("state discs: %d checks, %d failed\n", checks, fails); return fails != 0;
}
