// Finder log contract (plan.md, phase 7): scrub home paths across arbitrary byte boundaries, keep five runs, drain both streams at exit and restore their descriptors.
// Synthetic text and temporary files only; no Apple frameworks, game, BIOS or GPU. A run's final line need not have a newline.
#include "../log.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
typedef struct text { char bytes[32768]; size_t n; } text;
static int append(void *ctx, const char *p, size_t n) {
    text *t = ctx; if (t->n + n >= sizeof t->bytes) return -1; memcpy(t->bytes + t->n, p, n); t->n += n; t->bytes[t->n] = 0; return 0;
}
static int reject(void *ctx, const char *p, size_t n) { (void)ctx; (void)p; (void)n; return -1; }
static int count_logs(const char *directory) {
    DIR *d = opendir(directory); if (!d) return -1; int n = 0; struct dirent *entry;
    while ((entry = readdir(d))) if (!strncmp(entry->d_name, "run-", 4)) n++;
    closedir(d); return n;
}
int main(void) {
    const char *home = "/Users/Ó Name", *input = "host: /Users/Ó Name/a /Users/Ó Name\n/Users/Ó Nam";
    const char *expected = "host: ~/a ~\n/Users/Ó Nam";
    for (size_t cut = 0; cut <= strlen(input); cut++) {
        text out = {0}; front_log_filter f = {.home = home};
        int r = front_log_filter_write(&f, input, cut, 0, append, &out);
        r |= front_log_filter_write(&f, input + cut, strlen(input) - cut, 1, append, &out);
        CHECK(!r && !strcmp(out.bytes, expected), "every byte split, including UTF-8 and partial final prefixes, is redacted exactly");
    }
    text out = {0}; front_log_filter f = {.home = "/a/b"};
    const char *overlap = "/a/a/b/a/b";
    for (size_t i = 0; i < strlen(overlap); i++) front_log_filter_write(&f, overlap + i, 1, 0, append, &out);
    front_log_filter_write(&f, NULL, 0, 1, append, &out);
    CHECK(!strcmp(out.bytes, "/a~~"), "overlapping prefixes cannot leak a home path");
    out = (text){0}; f = (front_log_filter){.home = ""}; front_log_filter_write(&f, input, strlen(input), 1, append, &out);
    CHECK(!strcmp(out.bytes, input), "an absent home path preserves all bytes");
    f = (front_log_filter){.home = home};
    CHECK(front_log_filter_write(&f, "error", 5, 1, reject, NULL) == -1, "a failed log write is reported");

    char directory[] = "/tmp/muyiunative-log-XXXXXX"; CHECK(mkdtemp(directory) != NULL, "create an isolated log directory");
    char keep[PATH_MAX]; snprintf(keep, sizeof keep, "%s/keep.txt", directory); FILE *file = fopen(keep, "w"); if (file) { fputs("untouched", file); fclose(file); }
    char latest[PATH_MAX] = {0};
    for (int i = 0; i < 8; i++) {
        front_log s; int r = front_log_start(&s, directory, home, "host: muyiunative 0.7.0 (build 1), commit abc123\n");
        if (!r) {
            fputs("stdout: ", stdout); fflush(stdout);
            write(STDOUT_FILENO, home, 5); write(STDOUT_FILENO, home + 5, strlen(home) - 5);
            fputs("/disc\nstderr: failure\nfinal", stderr); front_log_stop(&s); strcpy(latest, s.path);
        }
        CHECK(!r && !s.failed, "both streams drain without losing the final unterminated line");
        CHECK(count_logs(directory) == (i < 5 ? i + 1 : 5), "retain exactly the last five runs");
    }
    file = fopen(latest, "r"); char contents[4096] = {0}; if (file) { fread(contents, 1, sizeof contents - 1, file); fclose(file); }
    CHECK(!strcmp(contents, "host: muyiunative 0.7.0 (build 1), commit abc123\nstdout: ~/disc\nstderr: failure\nfinal"), "the log starts with its version and retains stdout, stderr and redacted paths");
    struct stat st; CHECK(!stat(latest, &st) && (st.st_mode & 0777) == 0600, "each run log is private to its user");
    CHECK(!access(keep, F_OK), "retention preserves unrelated files");
    front_log failed;
    CHECK(front_log_start(&failed, "/nonexistent/muyiunative/logs", home, "header\n") == -1 && !failed.path[0], "creation failure leaves the output descriptors usable and no bogus log path");
    CHECK(fcntl(STDOUT_FILENO, F_GETFD) >= 0 && fcntl(STDERR_FILENO, F_GETFD) >= 0, "original output streams are restored after success and failure");
    front_log_stop(&failed);
    DIR *d = opendir(directory); struct dirent *entry;
    if (d) { while ((entry = readdir(d))) if (entry->d_name[0] != '.') { char path[PATH_MAX]; snprintf(path, sizeof path, "%s/%s", directory, entry->d_name); unlink(path); } closedir(d); }
    rmdir(directory);
    printf("log: %d checks, %d failed\n", checks, fails); return fails != 0;
}
