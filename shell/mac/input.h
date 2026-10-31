// Host input replays (plan.md, phase 4): parse ordered frame:hex-button events without a window or GPU. These are host inputs, not console state; each machine
// receives the same mask before its frame. Kept independent of Apple frameworks so the synthetic runtime test t_replay exercises long and malformed files in CI.
#pragma once
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct input { unsigned long frame; uint16_t buttons; } input;

// Long gameplay replays need thousands of button edges (a pistol fires on each press), beyond the old 64-event array. Keep the ordered event stream on the host;
// --input-file uses the same frame:hex-mask lines as --input, so capture, native playback and lockstep all receive exactly the same inputs.
static int front_input_add(input **events, size_t *n, size_t *cap, const char *s) {
    char *end; errno = 0; unsigned long frame = strtoul(s, &end, 10);
    if (errno || end == s || *end != ':' || frame > LONG_MAX) return -1;
    s = end + 1; errno = 0; unsigned long buttons = strtoul(s, &end, 16);
    if (errno || end == s || buttons > 0xffff) return -1;
    while (*end == '\r' || *end == '\n') end++;
    if (*end || (*n && frame < (*events)[*n - 1].frame)) return -1;
    if (*n == *cap) {
        size_t size = *cap ? 2 * *cap : 64;
        if (size < *cap || size > SIZE_MAX / sizeof **events) return -1;
        input *p = realloc(*events, size * sizeof *p); if (!p) return -1;
        *events = p; *cap = size;
    }
    (*events)[(*n)++] = (input){frame, (uint16_t)buttons}; return 0;
}

static int front_input_file(input **events, size_t *n, size_t *cap, const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char line[128]; int bad = 0;
    while (!bad && fgets(line, sizeof line, f)) bad = front_input_add(events, n, cap, line);
    if (ferror(f)) bad = -1;
    if (fclose(f)) bad = -1;
    return bad;
}
