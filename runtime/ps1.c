// The machine: creating and resetting it, loading its BIOS image, and running it a frame at a time (plan.md, phase 1)
#include "ps1.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

ps1 *ps1_new(void) {
    ps1 *m = calloc(1, sizeof *m);
    if (m) ps1_reset(m);
    return m;
}

void ps1_free(ps1 *m) { free(m); }

void ps1_reset(ps1 *m) {   // RAM, scratchpad and BIOS keep their contents, as on the console
    m->cycles = 0; m->i_stat = m->i_mask = m->ram_size = m->cache_ctl = m->post = 0;
    memset(m->memctl, 0, sizeof m->memctl);
    m->ntty = m->frame_done = m->exit_requested = m->exit_code = 0;
    cpu_reset(m); dma_reset(m); tmr_reset(m); gpu_reset(m); cd_reset(m); spu_reset(m); sio_reset(m);
    sched_update(m);
}

int ps1_load_bios(ps1 *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { RED("bios: cannot open %s: %s", path, strerror(errno)); return -1; }
    size_t n = fread(m->bios, 1, BIOS_SIZE, f); int more = fgetc(f) != EOF;
    fclose(f);
    if (n != BIOS_SIZE || more) { RED("bios: %s is not a 512 KB BIOS image (%s%zu bytes)", path, more ? "over " : "", n); return -1; }
    return 0;
}

void ps1_run_frame(ps1 *m) {
    m->frame_done = 0;
    while (!m->frame_done && !m->exit_requested) { cpu_run(m); sched_run(m); }
}
