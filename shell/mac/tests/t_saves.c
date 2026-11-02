// Phase 5 persistence uses only synthetic cards and temporary directories: fresh formatting, separate slots, durable reopening, collision locks, malformed-file
// refusal and a failed atomic replacement retaining the previous image. Card protocol and formatting are covered independently by t_sio; no game, BIOS or GPU.
#include "ps1.h"
#include "../saves.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static int byte(const char *path, int at) {
    FILE *f = fopen(path, "rb"); int v = -1;
    if (f) { if (!fseek(f, at, SEEK_SET)) v = fgetc(f); fclose(f); } return v;
}

int main(void) {
    char temp[] = "/tmp/muyiunative-saves-XXXXXX"; char *root = mkdtemp(temp);
    ps1 *m = ps1_new(), *other = ps1_new(); front_saves s = {-1, -1}, t = {-1, -1};
    if (!root || !m || !other) { fputs("FAIL: cannot create save test\n", stderr); ps1_free(m); ps1_free(other); return 1; }
    char dir[256], first[256], second[256], lock[256], moved[256];
    snprintf(dir, sizeof dir, "%s/nested/cards", root); snprintf(first, sizeof first, "%s/card1.mcr", dir);
    snprintf(second, sizeof second, "%s/card2.mcr", dir); snprintf(lock, sizeof lock, "%s/.lock", dir); snprintf(moved, sizeof moved, "%s/previous", dir);
    CHECK(!front_saves_open(&s, m, dir), "missing parents and card files are created");
    CHECK(m->sio.card[0].present && m->sio.card[1].present && byte(first, 0) == 'M' && byte(second, 1) == 'C', "both slots start as formatted raw cards");
    struct stat st; CHECK(!stat(first, &st) && st.st_size == CARD_SIZE && !m->sio.card[0].dirty, "initial card is durably flushed and clean");
    CHECK(front_saves_open(&t, other, dir) < 0 && t.dir_fd == -1 && !other->sio.card[0].present, "another running copy cannot share the files");
    m->sio.card[0].data[1234] = 0x42; m->sio.card[0].dirty = 1;
    m->sio.card[1].data[1234] = 0x24; m->sio.card[1].dirty = 1;
    CHECK(!front_saves_flush(&s, m) && !m->sio.card[0].dirty && !m->sio.card[1].dirty, "successful writes clear both dirty flags");
    CHECK(byte(first, 1234) == 0x42 && byte(second, 1234) == 0x24, "slot files preserve independent bytes");
    front_saves_close(&s);
    CHECK(!front_saves_open(&t, other, dir) && other->sio.card[0].data[1234] == 0x42 && other->sio.card[1].data[1234] == 0x24, "new machines reload persisted cards after the lock closes");
    CHECK(!rename(first, moved) && !mkdir(first, 0700), "a directory blocks the final atomic rename");
    other->sio.card[0].data[1234] = 0x99; other->sio.card[0].dirty = 1;
    CHECK(front_saves_flush(&t, other) < 0 && other->sio.card[0].dirty && byte(moved, 1234) == 0x42, "failed replacement retains the previous card and the dirty bytes for retry");
    CHECK(!rmdir(first) && !rename(moved, first) && !front_saves_flush(&t, other) && byte(first, 1234) == 0x99, "retry commits the pending image after the failure is removed");
    front_saves_close(&t);
    CHECK(!truncate(second, CARD_SIZE - 1), "synthetic image can be made malformed");
    CHECK(front_saves_open(&s, m, dir) < 0 && byte(first, 1234) == 0x99 && !stat(second, &st) && st.st_size == CARD_SIZE - 1, "malformed existing files are refused without formatting or replacing either slot");
    CHECK(!unlink(second) && !front_saves_open(&s, m, dir) && m->sio.card[0].data[1234] == 0x99 && m->sio.card[1].data[0] == 'M', "a missing second slot is formatted while the first slot is retained");
    front_saves_close(&s);
    CHECK(!unlink(second) && !symlink(first, second), "synthetic symlink image is installed");
    CHECK(front_saves_open(&s, m, dir) < 0 && byte(first, 1234) == 0x99, "card symlinks are refused without touching their target");
    front_saves_close(&s); front_saves_close(&t); ps1_free(m); ps1_free(other);
    unlink(first); unlink(second); unlink(lock); CHECK(!rmdir(dir), "failed writes leave no temporary card files behind");
    snprintf(dir, sizeof dir, "%s/nested", root); rmdir(dir); rmdir(root);
    printf("saves: %d checks, %d failed\n", checks, fails); return fails != 0;
}
