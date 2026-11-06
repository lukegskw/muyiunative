// A per-game quick state, owned by the machine thread, in the already locked memory-card directory. No host pointers or executable code are persisted.
#pragma once
#include <stdint.h>
struct ps1;
struct front_product;
enum { FRONT_STATE_ERROR = 256 };
typedef struct front_snapshot { struct ps1 *machine, *media; uint16_t *fine; uint64_t frame; int scale, disc_index; } front_snapshot;
int front_state_exists(int directory);
// scale is 0 (software/native) or 2/4 with a complete fine VRAM image. Atomic replacement never leaves a partially written slot; errors are described for the UI.
int front_state_save(int directory, const char *game, struct ps1 *m, uint64_t frame, int scale, const uint16_t *fine, char error[FRONT_STATE_ERROR]);
// Validate before touching the running console. The same game, BIOS, disc contents/layout and state format are required; the media identity is cached on first use.
front_snapshot *front_state_read(int directory, const char *game, struct ps1 *m, char error[FRONT_STATE_ERROR]);
// Bundled apps resolve the saved media by contents, staging any replacement without changing the running console. Existing format-2 states need no migration.
front_snapshot *front_state_read_bundled(int directory, const char *game, struct ps1 *m, const struct front_product *product, int current, char error[FRONT_STATE_ERROR]);
// Detach rendering before applying. A staged disc replaces the current streams; BIOS, callbacks, native code and cards stay attached. Consumes staged media.
void front_state_apply(struct ps1 *m, front_snapshot *state);
void front_state_free(front_snapshot *state);
