// Host-only run logs (plan.md, phase 7): both output streams enter one pipe; a reader removes the home path even across writes. No console state lives here.
#pragma once
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#define FRONT_LOG_LIMIT (10u * 1024u * 1024u)
typedef struct front_log_filter { const char *home; size_t n; char pending[PATH_MAX]; } front_log_filter;
typedef int (*front_log_output)(void *ctx, const char *bytes, size_t n);
int front_log_filter_write(front_log_filter *f, const char *bytes, size_t n, int final, front_log_output output, void *ctx);
typedef struct front_log {
    char path[PATH_MAX], home[PATH_MAX]; int file, read, saved[2], started, failed; pthread_t thread;
    size_t header, size; int truncated;
} front_log;
int front_log_start(front_log *s, const char *directory, const char *home, const char *header);
void front_log_stop(front_log *s);
