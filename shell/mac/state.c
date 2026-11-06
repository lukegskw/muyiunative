// Quick states resume at a frame boundary without replaying a game's opening. This is host persistence, not PS1 hardware behavior. Copy console fields explicitly:
// a diagnostic fwrite(ps1) checkpoint contains stale FILE/callback/native pointers and cannot be a player format. Native code stays compiled in the app.
// The payload is little-endian, format/ABI checked and checksummed; bump FORMAT when a device representation changes without changing its layout. No cross-version
// state compatibility is promised. External cards are retained on load; reject active card transactions rather than resume half a sector against newer card bytes.
#include "state.h"
#include "product.h"
#include "ps1.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONSOLE(X) X(ram) X(scratch) X(cpu) X(cycles) X(next_event) X(i_stat) X(i_mask) X(memctl) X(ram_size) X(cache_ctl) X(dpcr) X(dicr) X(post) \
    X(dma) X(dma_next) X(tmr) X(gpu) X(cd) X(spu) X(sio) X(mdec) X(tty) X(ntty) X(frame_done) X(exit_requested) X(exit_code)
enum { FORMAT = 2 };   // media identity now includes every track's contents, not only its layout
static const char filename[] = "quick.ps1state";
typedef struct header {
    char magic[8]; uint32_t version, bytes, scale, fine_bytes;
    uint64_t layout, bios, media, frame, checksum; char game[128];
} header;

static uint64_t hash(uint64_t h, const void *bytes, size_t n) {
    const uint8_t *p = bytes; while (n--) { h ^= *p++; h *= UINT64_C(0x100000001b3); } return h;
}
#define BASIS UINT64_C(0xcbf29ce484222325)
static uint64_t layout(void) {
    uint64_t h = BASIS;
#define FIELD(f) { const size_t a[] = {offsetof(ps1, f), sizeof ((ps1 *)0)->f}; h = hash(h, a, sizeof a); }
    CONSOLE(FIELD)
#undef FIELD
    return h;
}
static int media(ps1 *m, uint64_t *out) {
    uint64_t identity; if (disc_identity(m, &identity)) return -1;
    const ps1_disc *d = &m->disc; uint64_t h = hash(identity, &d->sectors, sizeof d->sectors);
    h = hash(h, &d->nfiles, sizeof d->nfiles); h = hash(h, &d->ntracks, sizeof d->ntracks);
    for (int i = 0; i < d->nfiles; i++) h = hash(h, &d->size[i], sizeof d->size[i]);
    for (int i = 0; i < d->ntracks; i++) {
        const ps1_disc_track *t = &d->track[i]; const uint32_t fields[] = {t->start, t->begin, t->offset, t->file, t->audio, t->mode};
        h = hash(h, fields, sizeof fields);   // no FILE pointers or compiler padding in the media identity
    }
    *out = h; return 0;
}
static void copy_console(ps1 *to, const ps1 *from) {
#define COPY(f) memcpy(&to->f, &from->f, sizeof to->f);
    CONSOLE(COPY)
#undef COPY
}
static int ready(const ps1 *m, char *error) {
    if (m->cd.shell_open) { snprintf(error, FRONT_STATE_ERROR, "Close the disc cover before saving or loading a state."); return 0; }
    if (m->sio.device == 2) { snprintf(error, FRONT_STATE_ERROR, "The memory card is busy. Try again after the game finishes accessing it."); return 0; }
    if (m->exe || m->native_active || m->on_step) { snprintf(error, FRONT_STATE_ERROR, "A state can only be saved or loaded at a normal game frame boundary."); return 0; }
    return 1;
}
static int io_error(char *error, const char *operation) {
    snprintf(error, FRONT_STATE_ERROR, "Could not %s the state: %s", operation, strerror(errno)); return -1;
}
static int transfer(int fd, void *data, size_t bytes, int writing) {
    uint8_t *p = data;
    while (bytes) {
        ssize_t n = writing ? write(fd, p, bytes) : read(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        p += n; bytes -= (size_t)n;
    }
    return 0;
}
static uint64_t checksum(header h, const ps1 *m, const uint16_t *fine) {
    h.checksum = 0; uint64_t v = hash(BASIS, &h, sizeof h); v = hash(v, m, sizeof *m); return hash(v, fine, h.fine_bytes);
}
int front_state_exists(int directory) {
    struct stat st; return !fstatat(directory, filename, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode);
}
int front_state_save(int directory, const char *game, ps1 *m, uint64_t frame, int scale, const uint16_t *fine, char *error) {
    if (!ready(m, error)) return -1;
    if (!game || !*game || strlen(game) >= 128 || (scale != 0 && scale != 2 && scale != 4) || (scale && !fine)) {
        snprintf(error, FRONT_STATE_ERROR, "The game or rendering state is invalid."); return -1;
    }
    uint64_t identity;
    if (media(m, &identity)) { snprintf(error, FRONT_STATE_ERROR, "Could not identify the mounted disc. Check that all track files are readable."); return -1; }
    ps1 *saved = calloc(1, sizeof *saved); if (!saved) return io_error(error, "allocate");
    copy_console(saved, m); memset(saved->sio.card, 0, sizeof saved->sio.card);
    header h = {.version = FORMAT, .bytes = sizeof *saved, .scale = (uint32_t)scale,
        .fine_bytes = (uint32_t)(sizeof m->gpu.vram * scale * scale), .layout = layout(), .bios = hash(BASIS, m->bios, sizeof m->bios),
        .media = identity, .frame = frame};
    memcpy(h.magic, "MYUSTATE", 8); memcpy(h.game, game, strlen(game)); h.checksum = checksum(h, saved, fine);
    char temp[80]; snprintf(temp, sizeof temp, ".quick-%ld-%08x.tmp", (long)getpid(), arc4random());
    int fd = openat(directory, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600), bad = fd < 0;
    if (!bad && (transfer(fd, &h, sizeof h, 1) || transfer(fd, saved, sizeof *saved, 1) || transfer(fd, (void *)fine, h.fine_bytes, 1) || fsync(fd))) bad = 1;
    int err = errno; free(saved);
    if (fd >= 0 && close(fd) && !bad) { err = errno; bad = 1; }
    if (!bad && renameat(directory, temp, directory, filename)) { err = errno; bad = 1; }
    if (!bad && fsync(directory)) { err = errno; bad = 1; }
    if (bad) { if (fd >= 0) unlinkat(directory, temp, 0); errno = err; return io_error(error, "save"); }
    return 0;
}
void front_state_free(front_snapshot *s) { if (s) { ps1_free(s->media); free(s->machine); free(s->fine); free(s); } }
static int select_media(front_snapshot *s, ps1 *m, const front_product *product, uint64_t saved, char *error) {
    uint64_t identity; int bad = media(m, &identity);
    if (!bad && identity == saved) return 0;
    if (!product || product->ndiscs < 1 || product->ndiscs > FRONT_DISC_MAX) {
        snprintf(error, FRONT_STATE_ERROR, bad ? "Could not identify the mounted disc. Check that all track files are readable." :
                 "This state belongs to a different disc revision. Insert the disc used when saving this state."); return -1;
    }
    // FFIX's Disc 2 state was rejected after an ordinary app restart because startup mounts Disc 1. Resolve the existing format-2 content identity, not its serial
    // or catalog order. Stage open streams separately: damaged states and missing/wrong revisions must never eject the live disc or disturb its controller.
    s->media = ps1_new(); if (!s->media) return io_error(error, "allocate");
    for (int i = 0; i < product->ndiscs; i++) {
        if (!disc_open(s->media, product->discs[i]) && !media(s->media, &identity) && identity == saved) { s->disc_index = i; return 0; }
    }
    snprintf(error, FRONT_STATE_ERROR, "The disc used by this state is not available in this app. Rebuild the app with the complete original disc set."); return -1;
}
static front_snapshot *read_state(int directory, const char *game, ps1 *m, const front_product *product, int current, char *error) {
    if (!ready(m, error)) return NULL;
    int fd = openat(directory, filename, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { io_error(error, "load"); return NULL; }
    header h; struct stat st; front_snapshot *s = NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < (off_t)sizeof h || transfer(fd, &h, sizeof h, 0)) goto invalid;
    if (memcmp(h.magic, "MYUSTATE", 8) || h.version != FORMAT || h.bytes != sizeof(ps1) || h.layout != layout() ||
        (h.scale != 0 && h.scale != 2 && h.scale != 4) || h.fine_bytes != sizeof m->gpu.vram * h.scale * h.scale ||
        st.st_size != (off_t)(sizeof h + sizeof(ps1) + h.fine_bytes)) goto invalid;
    if (!game || strlen(game) >= sizeof h.game || strncmp(h.game, game, sizeof h.game) || h.bios != hash(BASIS, m->bios, sizeof m->bios)) {
        snprintf(error, FRONT_STATE_ERROR, "This state belongs to a different game or BIOS."); goto fail;
    }
    s = calloc(1, sizeof *s);
    if (!s || !(s->machine = malloc(sizeof *s->machine)) || (h.fine_bytes && !(s->fine = malloc(h.fine_bytes)))) { io_error(error, "allocate"); goto fail; }
    if (transfer(fd, s->machine, sizeof *s->machine, 0) || transfer(fd, s->fine, h.fine_bytes, 0) || checksum(h, s->machine, s->fine) != h.checksum) goto invalid;
    if (s->machine->sio.device == 2 || s->machine->cd.shell_open) goto invalid;
    s->frame = h.frame; s->scale = (int)h.scale; s->disc_index = current;
    if (select_media(s, m, product, h.media, error)) goto fail;
    if (close(fd)) { io_error(error, "load"); front_state_free(s); return NULL; }
    return s;
invalid:
    snprintf(error, FRONT_STATE_ERROR, "The state is damaged or uses an incompatible format. Save a new state with this app.");
fail:
    close(fd); front_state_free(s); return NULL;
}
void front_state_apply(ps1 *m, front_snapshot *s) {
    // This replaces a saved timeline, not a live cover operation: restore its exact CD state after attaching the validated streams, with no reset/door interrupt.
    if (s->media) {
        ps1_disc old = m->disc; m->disc = s->media->disc; s->media->disc = old;
        ps1_free(s->media); s->media = NULL;
    }
    ps1_card cards[2]; memcpy(cards, m->sio.card, sizeof cards);
    copy_console(m, s->machine); memcpy(m->sio.card, cards, sizeof cards);
    m->native_active = NULL; m->native_dirty = 0;
}
front_snapshot *front_state_read_bundled(int directory, const char *game, ps1 *m, const struct front_product *product, int current, char *error) {
    return read_state(directory, game, m, product, current, error);
}
front_snapshot *front_state_read(int directory, const char *game, ps1 *m, char *error) { return read_state(directory, game, m, NULL, -1, error); }
