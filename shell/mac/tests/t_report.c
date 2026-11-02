// Phase 7's bug-form URL: field IDs, UTF-8, punctuation and buffer bounds, without a browser, frameworks or game data. Values are synthetic metadata.
#include "../report.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
int main(void) {
    front_report_info info = {"0.7.0 (build 123)", "abcdef123456", "Mac16,8, macOS 26.6.2", "SLUS-00001", "Metal (4×)"}; char url[4096];
    CHECK(!front_report_url(url, sizeof url, &info), "construct a complete issue-form URL");
    CHECK(strstr(url, "/issues/new?template=bug_report.yml") != NULL && !strstr(url, "&body="), "select the actual bug form without hiding field values in its body");
    CHECK(strstr(url, "&app-version=0.7.0%20%28build%20123%29&commit=abcdef123456&device=Mac16%2C8%2C%20macOS%2026.6.2&game=SLUS-00001&renderer=Metal%20%284%C3%97%29") != NULL, "version, commit, Mac/macOS, serial and renderer each use their own form field ID");
    size_t n = strlen(url); char exact[4096];
    CHECK(!front_report_url(exact, n + 1, &info) && !strcmp(exact, url), "the exact capacity includes a terminator");
    for (size_t cap = 0; cap <= n; cap++) {
        memset(exact, '#', sizeof exact); int r = front_report_url(exact, cap, &info);
        CHECK(r == -1 && exact[cap] == '#', "every undersized buffer fails without an out-of-bounds write");
    }
    info.game = "A+B & C#D?/漢字"; CHECK(!front_report_url(url, sizeof url, &info) && strstr(url, "game=A%2BB%20%26%20C%23D%3F%2F%E6%BC%A2%E5%AD%97&renderer="), "reserved characters and non-ASCII bytes cannot change query fields");
    info.game = NULL; CHECK(!front_report_url(url, sizeof url, &info) && strstr(url, "&game=&renderer="), "a launcher or unknown disc can leave the game field empty");
    printf("report: %d checks, %d failed\n", checks, fails); return fails != 0;
}
