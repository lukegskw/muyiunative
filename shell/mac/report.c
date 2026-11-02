// GitHub issue-form query parameters (plan.md, phase 7; docs.github.com, Creating an issue from a URL query): one key per form field ID.
// Encode every byte except RFC 3986 unreserved characters: '+' must survive as %2B, not become a space, and UTF-8 is encoded byte by byte.
#include "report.h"
#include <stdio.h>
#include <string.h>
int front_report_url(char *out, size_t cap, const front_report_info *info) {
    const char *base = "https://github.com/lukegskw/muyiunative/issues/new?template=bug_report.yml";
    size_t used = strlen(base); if (used >= cap) return -1; memcpy(out, base, used + 1);
    const char *keys[] = {"app-version", "commit", "device", "game", "renderer"};
    const char *values[] = {info->version, info->commit, info->device, info->game, info->renderer};
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) {
        size_t n = strlen(keys[i]); if (used + n + 2 >= cap) return -1;
        out[used++] = '&'; memcpy(out + used, keys[i], n); used += n; out[used++] = '=';
        for (const unsigned char *p = (const unsigned char *)(values[i] ? values[i] : ""); *p; p++) {
            int plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p);
            size_t add = plain ? 1 : 3; if (used + add >= cap) return -1;
            if (plain) out[used++] = (char)*p;
            else { static const char hex[] = "0123456789ABCDEF"; out[used++] = '%'; out[used++] = hex[*p >> 4]; out[used++] = hex[*p & 15]; }
        }
        out[used] = 0;
    }
    return 0;
}
