// Host replays for phase 4 (plan.md): thousands of edges, same-frame changes, malformed masks and ordering. The parser has no Apple-framework dependency, so gameplay
// input handling is checked in CI without a GPU. Button masks are passed unchanged to each machine's digital pad (psx-spx, Controllers).
#include "../input.h"
#include <string.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

int main(void) {
    input *events = NULL; size_t n = 0, cap = 0;
    CHECK(!front_input_add(&events, &n, &cap, "0:8") && !front_input_add(&events, &n, &cap, "0:0"), "same-frame edges keep their order");
    CHECK(n == 2 && events[0].buttons == 8 && !events[1].buttons, "masks reach the host unchanged");
    const char *bad[] = {"0:", "0:\n", "0:10000", "-1:8", "0:8 garbage", "0x10:8", "8", "0:8:0"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(front_input_add(&events, &n, &cap, bad[i]) == -1 && n == 2, "malformed inputs do not append an event");
    CHECK(!front_input_add(&events, &n, &cap, "10:ffff\r\n") && front_input_add(&events, &n, &cap, "9:0") == -1, "CRLF is accepted, unordered events are refused");
    free(events); events = NULL; n = cap = 0;
    char path[] = "/tmp/ps1native-input-XXXXXX"; int fd = mkstemp(path); FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    CHECK(f, "temporary replay file");
    if (f) {
        for (unsigned i = 0; i < 3000; i++) fprintf(f, "%u:%x\n", i, i & 1 ? 0x8000 : 0x20);
        fclose(f);
        CHECK(!front_input_file(&events, &n, &cap, path), "the host accepts a long gameplay replay");
        CHECK(n == 3000 && events[2999].frame == 2999 && events[2999].buttons == 0x8000, "every edge survives buffer growth beyond 64 events");
        free(events); events = NULL; n = cap = 0;
        f = fopen(path, "w"); if (f) { fputs("1:8\n0:0\n", f); fclose(f); }
        CHECK(front_input_file(&events, &n, &cap, path) == -1, "unordered file input fails instead of silently changing the replay");
    }
    unlink(path); free(events);
    printf("replay: %d checks, %d failed\n", checks, fails); return fails != 0;
}
