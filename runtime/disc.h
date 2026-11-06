// External raw discs (plan.md, phase 2): TOC and read-only streams live in the machine. psx-spx, CDROM File Formats, "CDROM Disk Images CUE/BIN/CDT (Cdrwin)"
#pragma once
#include <stdint.h>
#include <stdio.h>

enum { DISC_SECTOR = 2352, DISC_TRACKS = 99 };
typedef struct ps1_disc_track { uint32_t start, begin, offset; uint8_t file, audio, mode; } ps1_disc_track;
typedef struct ps1_disc {
    FILE *file[DISC_TRACKS]; uint32_t size[DISC_TRACKS], sectors;
    ps1_disc_track track[DISC_TRACKS]; int nfiles, ntracks;
    uint64_t identity; int identified;   // lazy content identity for host quick states, never serialized as console state
} ps1_disc;
struct ps1;
int disc_open(struct ps1 *m, const char *path);   // 0 or -1; failure leaves the old mount intact; never logs disc contents or names. Open the cover before live replacement
void disc_close(struct ps1 *m);
int disc_track(const struct ps1 *m, uint32_t lba);   // zero-based track, including INDEX00, or -1 outside the disc
int disc_read(struct ps1 *m, uint32_t lba, uint8_t out[DISC_SECTOR]);   // raw bytes; 0 or -1
int disc_identity(struct ps1 *m, uint64_t *out);   // hash complete track files once per mount, preserving stream positions; 0 or -1 on I/O failure
