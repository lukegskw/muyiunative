// Player disc changes preserve a running console, native bindings and shared memory cards, including failure/cancel-equivalent closure paths. No game inputs.
#include "native.h"
#include "../media.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static const ps1_native_set native = {.name = "synthetic"};
int main(void) {
    ps1 *m = ps1_new(); char path[] = "/tmp/muyiunative-media-XXXXXX.bin", error[FRONT_DISC_ERROR];
    int fd = mkstemps(path, 4); if (!m || fd < 0) return 1;
    uint8_t bytes[DISC_SECTOR] = {0}; memset(bytes + 1, 0xff, 10); bytes[15] = 2; bytes[100] = 0x71;
    if (write(fd, bytes, sizeof bytes) != sizeof bytes) return 1; close(fd);
    CHECK(!disc_open(m, path), "initial synthetic disc mounts");
    m->cycles = 12345; m->cpu.r[3] = 0x5abc; m->ram[100] = 0x92; m->gpu.vram[100] = 0x5432; m->spu.ram[200] = 0x67;
    sio_card_format(m, 0); m->sio.card[0].data[500] = 0x32; m->sio.card[0].dirty = 1; ps1_native_add(m, &native);
    FILE *first = m->disc.file[0];
    CHECK(front_disc_apply(m, FRONT_DISC_INSERT, path, error) < 0 && m->disc.file[0] == first, "a live insert requires an open cover");
    CHECK(!front_disc_apply(m, FRONT_DISC_OPEN, NULL, error) && m->cd.shell_open, "menu opens the physical cover");
    CHECK(front_disc_apply(m, FRONT_DISC_INSERT, "/missing/synthetic.cue", error) < 0 && m->cd.shell_open && m->disc.file[0] == first,
          "a failed insertion preserves the old streams and leaves the cover open for recovery");
    CHECK(!front_disc_apply(m, FRONT_DISC_CLOSE, NULL, error) && !m->cd.shell_open && m->disc.file[0] == first, "cancelled selection can close the cover with the original disc");
    CHECK(!front_disc_apply(m, FRONT_DISC_OPEN, NULL, error) && !front_disc_apply(m, FRONT_DISC_INSERT, path, error) && !m->cd.shell_open,
          "successful insertion mounts read-only and closes the cover");
    CHECK(m->cycles == 12345 && m->cpu.r[3] == 0x5abc && m->ram[100] == 0x92 && m->gpu.vram[100] == 0x5432 && m->spu.ram[200] == 0x67,
          "CPU, RAM, GPU, SPU and guest time survive the swap");
    CHECK(m->native[0] == &native && m->sio.card[0].data[500] == 0x32 && m->sio.card[0].dirty, "native code and the same dirty memory-card saves remain attached");
    CHECK(!disc_read(m, 0, bytes) && bytes[100] == 0x71, "newly mounted streams remain usable");
    CHECK(front_disc_apply(m, FRONT_DISC_NONE, NULL, error) < 0, "unknown requests fail without touching the console");
    ps1_free(m); unlink(path); printf("media: %d checks, %d failed\n", checks, fails); return fails != 0;
}
