// Phase 7's Finder log. Observation: Finder's non-terminal stdout/stderr discarded the host's errors; retain both in a per-run file, redacting the home directory.
// POSIX pipes, dup2 and pthread_join drain even an unterminated final line at normal exit. The five-run retention and UTF-8 text format were approved 2026-11-02.
#include "log.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int write_all(void *ctx, const char *p, size_t n) {
    int fd = *(int *)ctx;
    while (n) { ssize_t w = write(fd, p, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) return -1; p += w; n -= (size_t)w; }
    return 0;
}
int front_log_filter_write(front_log_filter *f, const char *bytes, size_t n, int final, front_log_output output, void *ctx) {
    char out[4096]; size_t used = 0, home = strlen(f->home);
    if (home >= sizeof f->pending) return -1;
    for (size_t i = 0; i < n; i++) {
        f->pending[f->n++] = bytes[i];
        // Hold only a possible home-prefix. Retrying each suffix also catches overlapping prefixes such as /Users/Users/name.
        while (f->n && (!home || memcmp(f->pending, f->home, f->n))) {
            out[used++] = f->pending[0]; memmove(f->pending, f->pending + 1, --f->n);
            if (used == sizeof out) { if (output(ctx, out, used)) return -1; used = 0; }
        }
        if (home && f->n == home) { out[used++] = '~'; f->n = 0; }
        if (used == sizeof out) { if (output(ctx, out, used)) return -1; used = 0; }
    }
    if (used && output(ctx, out, used)) return -1;
    if (final && f->n) { if (output(ctx, f->pending, f->n)) return -1; f->n = 0; }
    return 0;
}
static void *read_log(void *arg) {
    front_log *s = arg; front_log_filter f = {.home = s->home}; char bytes[4096];
    for (;;) {
        ssize_t n = read(s->read, bytes, sizeof bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n < 0) s->failed = 1; break; }
        if (front_log_filter_write(&f, bytes, (size_t)n, 0, write_all, &s->file)) s->failed = 1;
    }
    if (front_log_filter_write(&f, NULL, 0, 1, write_all, &s->file)) s->failed = 1;
    return NULL;
}
static int log_name(const struct dirent *d) { size_t n = strlen(d->d_name); return !strncmp(d->d_name, "run-", 4) && n > 8 && !strcmp(d->d_name + n - 4, ".log"); }
static int rotate(const char *directory) {
    struct dirent **names; int n = scandir(directory, &names, log_name, alphasort), r = 0;
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        if (i < n - 5) { char path[PATH_MAX]; if (snprintf(path, sizeof path, "%s/%s", directory, names[i]->d_name) >= (int)sizeof path || unlink(path)) r = -1; }
        free(names[i]);
    }
    free(names); return r;
}
int front_log_start(front_log *s, const char *directory, const char *home, const char *header) {
    *s = (front_log){.file = -1, .read = -1, .saved = {-1, -1}};
    if (strlen(home) >= sizeof s->home) return -1;
    strcpy(s->home, home);
    struct timespec now; struct tm utc; char stamp[32];
    if (clock_gettime(CLOCK_REALTIME, &now) || !gmtime_r(&now.tv_sec, &utc) || !strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%S", &utc)) return -1;
    if (snprintf(s->path, sizeof s->path, "%s/run-%s-%09ld-%ld-XXXXXX.log", directory, stamp, now.tv_nsec, (long)getpid()) >= (int)sizeof s->path) { s->path[0] = 0; return -1; }
    int pipefd[2] = {-1, -1};
    s->file = mkstemps(s->path, 4);
    if (s->file < 0) goto fail;
    fcntl(s->file, F_SETFD, FD_CLOEXEC);
    front_log_filter f = {.home = s->home};
    if (front_log_filter_write(&f, header, strlen(header), 1, write_all, &s->file) || pipe(pipefd)) goto fail;
    s->read = pipefd[0]; fcntl(s->read, F_SETFD, FD_CLOEXEC); fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    fflush(NULL);
    for (int i = 0; i < 2; i++) { s->saved[i] = dup(i + 1); if (s->saved[i] < 0) goto fail; fcntl(s->saved[i], F_SETFD, FD_CLOEXEC); }
    if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) goto fail;
    close(pipefd[1]); pipefd[1] = -1;
    if (pthread_create(&s->thread, NULL, read_log, s)) goto fail;
    s->started = 1;
    if (rotate(directory)) fprintf(stderr, "[RED] host: could not remove an old run log\n");
    return 0;
fail:
    for (int i = 0; i < 2; i++) if (s->saved[i] >= 0) { dup2(s->saved[i], i + 1); close(s->saved[i]); s->saved[i] = -1; }
    if (pipefd[1] >= 0) close(pipefd[1]);
    if (s->read >= 0) close(s->read);
    if (s->file >= 0) { close(s->file); unlink(s->path); }
    s->path[0] = 0; return -1;
}
void front_log_stop(front_log *s) {
    if (!s->started) return;
    fflush(NULL);
    for (int i = 0; i < 2; i++) { dup2(s->saved[i], i + 1); close(s->saved[i]); }
    pthread_join(s->thread, NULL); close(s->read); close(s->file); s->started = 0;
    if (s->failed) fputs("[RED] host: could not write the complete run log\n", stderr);
}
