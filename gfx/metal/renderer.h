// Metal renderer (plan.md, phase 6): opaque per-machine host resources and an offline metallib. Native VRAM/cache are independent of the software reference.
#pragma once
#include "ps1.h"
typedef struct metal_renderer metal_renderer;
metal_renderer *metal_renderer_new(ps1 *m, const char *library, int scale);   // attach to m, scale=1,2,4; NULL after [RED] on failure
void metal_renderer_free(metal_renderer *r);   // detach before freeing m
int metal_renderer_frame(metal_renderer *r, uint32_t *out, int cap_w, int cap_h, int *w, int *h, int verify); // render/synchronize; verify full native VRAM/cache
int metal_renderer_check(metal_renderer *r);   // synchronize and compare full native VRAM/cache to its attached machine
const uint16_t *metal_renderer_vram(metal_renderer *r);   // synchronize; test-only readback, owned by renderer
