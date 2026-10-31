// Memory-card persistence outside the app bundle (plan.md, phase 5). The runtime already marks successful SIO writes dirty; leaving the slots disconnected in
// main.c lost them at exit. Load exact raw images, format only missing files, and replace each dirty image atomically so a failed write keeps the last good file.
// Card layout/formatting stays in sio.c (psx-spx, Controllers and Memory Cards, Data Format). These are host files, never runtime or game-specific hardware rules.
#include "saves.h"
#include "ps1.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int directories(const char *path) {
    char b[PATH_MAX]; size_t n = strlen(path);
    if (!n || n >= sizeof b) { errno = ENAMETOOLONG; return -1; }
    memcpy(b, path, n + 1);
    for (char *p = b + 1; ; p++) if (!*p || *p == '/') {
        char c = *p; *p = 0;
        if (mkdir(b, 0700) && errno != EEXIST) return -1;
        *p = c; if (!c) break;
    }
    return 0;
}

void front_saves_close(front_saves *s) {
    if (s->lock_fd >= 0) close(s->lock_fd);
    if (s->dir_fd >= 0) close(s->dir_fd);
    s->dir_fd = s->lock_fd = -1;
}

static int load(front_saves *s, int port, uint8_t *image) {
    char name[16]; snprintf(name, sizeof name, "card%d.mcr", port + 1);
    int fd = openat(s->dir_fd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC); struct stat st;
    if (fd < 0) { if (errno == ENOENT) return 1; return -1; }
    int bad = fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != CARD_SIZE;
    if (bad) errno = EINVAL;
    size_t n = 0;
    while (!bad && n < CARD_SIZE) {
        ssize_t r = read(fd, image + n, CARD_SIZE - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (!r) errno = EIO; bad = 1; break; }
        n += (size_t)r;
    }
    int err = errno; if (close(fd) && !bad) { err = errno; bad = 1; } errno = err;
    return bad ? -1 : 0;
}

int front_saves_flush(front_saves *s, ps1 *m) {
    if (s->dir_fd < 0 || s->lock_fd < 0) { RED("saves: cards are not open"); return -1; }
    for (int i = 0; i < 2; i++) {
        ps1_card *c = &m->sio.card[i]; if (!c->present || !c->dirty) continue;
        char name[16], temp[64]; snprintf(name, sizeof name, "card%d.mcr", i + 1);
        snprintf(temp, sizeof temp, ".card%d-%ld-%08x.tmp", i + 1, (long)getpid(), arc4random());
        int fd = openat(s->dir_fd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600), bad = fd < 0;
        size_t n = 0;
        while (!bad && n < CARD_SIZE) {
            ssize_t w = write(fd, c->data + n, CARD_SIZE - n);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { if (!w) errno = EIO; bad = 1; break; }
            n += (size_t)w;
        }
        if (!bad && fsync(fd)) bad = 1;
        int err = errno;
        if (fd >= 0 && close(fd) && !bad) { err = errno; bad = 1; }
        if (!bad && renameat(s->dir_fd, temp, s->dir_fd, name)) { err = errno; bad = 1; }
        if (!bad && fsync(s->dir_fd)) { err = errno; bad = 1; }
        if (bad) { if (fd >= 0) unlinkat(s->dir_fd, temp, 0); RED("saves: cannot save card %d: %s", i + 1, strerror(err)); return -1; }
        c->dirty = 0;
    }
    return 0;
}

int front_saves_open(front_saves *s, ps1 *m, const char *directory) {
    *s = (front_saves){-1, -1}; uint8_t *images = NULL; int missing[2]; struct stat st;
    if (directories(directory) || (s->dir_fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0 ||
        (s->lock_fd = openat(s->dir_fd, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600)) < 0 ||
        fstat(s->lock_fd, &st)) goto bad;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto bad; }
    if (flock(s->lock_fd, LOCK_EX | LOCK_NB)) { RED("saves: another running copy is using these memory cards"); goto fail; }
    images = malloc(2 * CARD_SIZE); if (!images) goto bad;
    for (int i = 0; i < 2; i++) if ((missing[i] = load(s, i, images + i * CARD_SIZE)) < 0) {
        RED("saves: cannot load card %d (expected a raw 128KB image): %s", i + 1, strerror(errno)); goto fail;
    }
    for (int i = 0; i < 2; i++) {
        if (missing[i]) sio_card_format(m, i); else sio_card_insert(m, i, images + i * CARD_SIZE, CARD_SIZE);
    }
    free(images); images = NULL;
    if (front_saves_flush(s, m)) goto fail;
    return 0;
bad:
    RED("saves: cannot open memory-card directory: %s", strerror(errno));
fail:
    free(images); front_saves_close(s); return -1;
}
