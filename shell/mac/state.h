// A per-game quick state, owned by the machine thread, in the already locked memory-card directory. No host pointers or executable code are persisted.
#pragma once
#include <stdint.h>
struct ps1;
enum { FRONT_STATE_ERROR = 256 };
typedef struct front_snapshot { struct ps1 *machine; uint16_t *fine; uint64_t frame; int scale; } front_snapshot;
int front_state_exists(int directory);
// scale is 0 (software/native) or 2/4 with a complete fine VRAM image. Atomic replacement never leaves a partially written slot; errors are described for the UI.
int front_state_save(int directory, const char *game, const struct ps1 *m, uint64_t frame, int scale, const uint16_t *fine, char error[FRONT_STATE_ERROR]);
// Validate before touching the running machine. The same game, BIOS, disc layout and state format are required.
front_snapshot *front_state_read(int directory, const char *game, const struct ps1 *m, char error[FRONT_STATE_ERROR]);
// Detach rendering before applying. Current BIOS, disc handles, callbacks, native code and memory cards remain attached; only console state rewinds.
void front_state_apply(struct ps1 *m, const front_snapshot *state);
void front_state_free(front_snapshot *state);
