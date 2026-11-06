// Host disc changes at frame boundaries. Keep the original app/save identity and attached native sets; only the drive's cover and mounted streams change.
#pragma once
struct ps1;
enum { FRONT_DISC_NONE, FRONT_DISC_OPEN, FRONT_DISC_CLOSE, FRONT_DISC_INSERT, FRONT_DISC_ERROR = 256 };
int front_disc_apply(struct ps1 *m, int action, const char *path, char error[FRONT_DISC_ERROR]);
