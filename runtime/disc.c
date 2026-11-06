// Read-only raw disc streams (plan.md, phase 2). Evidence: the synthetic t_disc changes byte patterns at file/track boundaries, where treating CUE times as disc times
// addresses the wrong sector. psx-spx, CDROM File Formats, "CDROM Disk Images CUE/BIN/CDT (Cdrwin)": INDEX times address the BIN; 2352-byte sectors omit the first 150
// physical sectors. INDEX00 is stored pregap; INDEX01 starts the track. Missing PREGAP/POSTGAP sectors and non-2352 formats are explicitly refused in this first step
// https://psx-spx.consoledev.net/cdromfileformats/#cdrom-disk-images-cuebincdt-cdrwin
#include "ps1.h"
#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <strings.h>

static void close_disc(ps1_disc *d) { for (int i = 0; i < d->nfiles; i++) fclose(d->file[i]); memset(d, 0, sizeof *d); }
void disc_close(ps1 *m) { close_disc(&m->disc); cd_reset(m); sched_update(m); }

static int open_bin(ps1_disc *d, const char *path) {
    if (d->nfiles == DISC_TRACKS) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    // t_disc truncated an open image and a seek within stdio's read-ahead returned stale bytes. Each sector read must reach the external stream so I/O failure is visible
    if (setvbuf(f, NULL, _IONBF, 0) || fseeko(f, 0, SEEK_END)) { fclose(f); return -1; }
    off_t n = ftello(f);
    if (n <= 0 || n % DISC_SECTOR || n / DISC_SECTOR > 100 * 60 * 75) { fclose(f); return -1; }
    d->size[d->nfiles] = (uint32_t)(n / DISC_SECTOR); d->file[d->nfiles++] = f;
    return 0;
}

static char *token(char **p) {   // words or quoted names; semicolon comments need no interpretation of disc-derived strings
    while (isspace((unsigned char)**p)) (*p)++;
    if (!**p || **p == ';') return NULL;
    char *s = *p;
    if (*s == '"') {
        s = ++*p; while (**p && **p != '"') (*p)++;
        if (!**p || ((*p)[1] && !isspace((unsigned char)(*p)[1]) && (*p)[1] != ';')) return *p;
        *(*p)++ = 0;
    } else { while (**p && !isspace((unsigned char)**p) && **p != ';') (*p)++; if (**p == ';') **p = 0; else if (**p) *(*p)++ = 0; }
    return s;
}
static int number(const char *s, unsigned *n) {
    if (!s || !*s) return -1;
    unsigned v = 0;
    for (; *s; s++) { if (*s < '0' || *s > '9' || v > 10000) return -1; v = v * 10 + (unsigned)(*s - '0'); }
    *n = v; return 0;
}
static int timecode(const char *s, uint32_t *out) {
    if (!s || strlen(s) != 8 || s[2] != ':' || s[5] != ':') return -1;
    for (int i = 0; i < 8; i++) if (i != 2 && i != 5 && (s[i] < '0' || s[i] > '9')) return -1;
    unsigned mm = (unsigned)(s[0] - '0') * 10 + s[1] - '0', ss = (unsigned)(s[3] - '0') * 10 + s[4] - '0', ff = (unsigned)(s[6] - '0') * 10 + s[7] - '0';
    if (ss > 59 || ff > 74) return -1;
    *out = (mm * 60 + ss) * 75 + ff; return 0;
}

static int cue(ps1_disc *d, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[PATH_MAX + 128], joined[PATH_MAX]; int bad = 0, last_file = -1;
    while (!bad && fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && !feof(f)) { bad = 1; break; }
        char *p = line, *cmd = token(&p), *a, *b;
        if (!cmd) { while (isspace((unsigned char)*p)) p++; if (*p && *p != ';') bad = 1; continue; }
        if (!strcmp(cmd, "REM") || !strcmp(cmd, "TITLE") || !strcmp(cmd, "PERFORMER") || !strcmp(cmd, "SONGWRITER") || !strcmp(cmd, "CATALOG") || !strcmp(cmd, "ISRC")) continue;
        a = token(&p); b = token(&p);
        if (!strcmp(cmd, "FILE")) {
            if (!a || !*a || !b || strcmp(b, "BINARY") || token(&p) || (d->nfiles && last_file != d->nfiles - 1)) { bad = 1; break; }
            for (char *q = a; *q; q++) if (*q == '\\') *q = '/';
            const char *slash = strrchr(path, '/'); size_t dir = slash ? (size_t)(slash - path + 1) : 0;
            if (*a == '/') dir = 0;
            if (dir + strlen(a) >= sizeof joined) { bad = 1; break; }
            memcpy(joined, path, dir); strcpy(joined + dir, a); bad = open_bin(d, joined) != 0;
        } else if (!strcmp(cmd, "TRACK")) {
            unsigned n;
            if (number(a, &n) || !b || token(&p) || !d->nfiles || n != (unsigned)d->ntracks + 1 || n > DISC_TRACKS ||
                (strcmp(b, "AUDIO") && strcmp(b, "MODE1/2352") && strcmp(b, "MODE2/2352"))) { bad = 1; break; }
            d->track[d->ntracks++] = (ps1_disc_track){UINT32_MAX, UINT32_MAX, 0, (uint8_t)(d->nfiles - 1), !strcmp(b, "AUDIO"), !strcmp(b, "AUDIO") ? 0 : (uint8_t)(b[4] - '0')};
            last_file = d->nfiles - 1;
        } else if (!strcmp(cmd, "INDEX")) {
            unsigned n; uint32_t at;
            if (!d->ntracks || number(a, &n) || n > 1 || timecode(b, &at) || token(&p)) { bad = 1; break; }
            ps1_disc_track *t = &d->track[d->ntracks - 1];
            if (t->file != d->nfiles - 1 || (n == 0 && (t->begin != UINT32_MAX || t->start != UINT32_MAX)) || (n == 1 && t->start != UINT32_MAX)) { bad = 1; break; }
            if (n) t->start = at; else t->begin = at;
        } else bad = 1;   // unsupported layout directives fail instead of silently shifting the disc
    }
    if (ferror(f)) bad = 1;
    fclose(f);
    if (bad || !d->ntracks || last_file != d->nfiles - 1) return -1;
    int64_t base = -(int64_t)d->track[0].start;
    for (int i = 0; i < d->ntracks; i++) {
        ps1_disc_track *t = &d->track[i]; int file = t->file;
        if (t->start == UINT32_MAX || t->start >= d->size[file]) return -1;
        if (t->begin == UINT32_MAX) t->begin = t->start;
        if (t->begin > t->start) return -1;
        if (i && file != d->track[i - 1].file) { base += d->size[file - 1]; if (t->begin) return -1; }
        else if (i && t->begin <= d->track[i - 1].offset + d->track[i - 1].start - d->track[i - 1].begin) return -1;
        t->offset = t->begin; t->start = (uint32_t)(base + t->start);
        if (!i) { t->begin = 0; t->offset = (uint32_t)-base; }
        else t->begin = (uint32_t)(base + t->begin);
    }
    d->sectors = (uint32_t)(base + d->size[d->nfiles - 1]);
    return d->sectors <= 100 * 60 * 75 ? 0 : -1;
}

int disc_open(ps1 *m, const char *path) {
    ps1_disc d = {0}; const char *ext = strrchr(path, '.'); int bad = 1;
    if (ext && !strcasecmp(ext, ".cue")) bad = cue(&d, path);
    else if (ext && !strcasecmp(ext, ".bin")) {
        bad = open_bin(&d, path);
        if (!bad) {
            uint8_t head[16];
            bad = fseeko(d.file[0], 0, SEEK_SET) || fread(head, 1, sizeof head, d.file[0]) != sizeof head;
            if (!bad) {
                bad = head[0] || head[11] || (head[15] != 1 && head[15] != 2);
                for (int i = 1; i < 11; i++) if (head[i] != 0xff) bad = 1;
            }
            if (!bad) { d.ntracks = 1; d.sectors = d.size[0]; d.track[0] = (ps1_disc_track){.mode = head[15]}; }
        }
    }
    if (bad) { close_disc(&d); RED("disc: cannot mount image (missing, malformed, or unsupported layout)"); return -1; }
    close_disc(&m->disc); m->disc = d;
    if (!m->cd.shell_open) cd_reset(m);   // a live replacement with the cover open must retain IRQ masks, mode, volume and the door notification
    sched_update(m);
    return 0;
}

int disc_track(const ps1 *m, uint32_t lba) {
    const ps1_disc *d = &m->disc;
    if (!d->ntracks || lba >= d->sectors) return -1;
    for (int i = d->ntracks - 1; i >= 0; i--) if (lba >= d->track[i].begin) return i;
    return -1;
}
int disc_read(ps1 *m, uint32_t lba, uint8_t out[DISC_SECTOR]) {
    int i = disc_track(m, lba);
    if (i < 0) return -1;
    const ps1_disc_track *t = &m->disc.track[i]; FILE *f = m->disc.file[t->file];
    off_t at = (off_t)(t->offset + lba - t->begin) * DISC_SECTOR;
    return fseeko(f, at, SEEK_SET) || fread(out, 1, DISC_SECTOR, f) != DISC_SECTOR ? -1 : 0;
}

int disc_identity(ps1 *m, uint64_t *out) {
    ps1_disc *d = &m->disc;
    if (!d->identified) {
        uint64_t h = UINT64_C(0xcbf29ce484222325); uint8_t bytes[65536];
        // t_state mounts equal-length discs with different bytes: a TOC alone accepts the wrong disc. Hash all bytes, including later data/audio tracks.
        for (int i = 0; i < d->nfiles; i++) {
            FILE *f = d->file[i]; off_t position = ftello(f); uint64_t left = (uint64_t)d->size[i] * DISC_SECTOR; int bad = position < 0;
            if (!bad && fseeko(f, 0, SEEK_SET)) bad = 1;
            while (!bad && left) {
                size_t n = left < sizeof bytes ? (size_t)left : sizeof bytes;
                if (fread(bytes, 1, n, f) != n) { bad = 1; break; }
                for (size_t j = 0; j < n; j++) { h ^= bytes[j]; h *= UINT64_C(0x100000001b3); }
                left -= n;
            }
            if (!bad && (fgetc(f) != EOF || ferror(f))) bad = 1;
            clearerr(f);
            if (position >= 0 && fseeko(f, position, SEEK_SET)) bad = 1;
            if (bad) return -1;
        }
        d->identity = h; d->identified = 1;
    }
    *out = d->identity; return 0;
}
