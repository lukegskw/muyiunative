// Metal renderer (plan.md, phase 6): opaque per-machine host resources and an offline metallib. Native VRAM/cache are independent of the software reference.
#pragma once
#include "ps1.h"
typedef struct metal_renderer metal_renderer;
metal_renderer *metal_renderer_new(ps1 *m, const char *library, int scale);   // attach to m, scale=1,2,4; NULL after [RED] on failure
void metal_renderer_free(metal_renderer *r);   // detach before freeing m
int metal_renderer_frame(metal_renderer *r, uint32_t *out, int cap_w, int cap_h, int *w, int *h, int verify); // render/synchronize; verify full native VRAM/cache
int metal_renderer_frame_async(metal_renderer *r, uint32_t *out, int cap_w, int cap_h, int *w, int *h); // submit this frame, return two frames earlier; first two calls w=h=0
int metal_renderer_check(metal_renderer *r);   // synchronize and compare full native VRAM/cache to its attached machine
const uint16_t *metal_renderer_vram(metal_renderer *r);   // synchronize; test-only readback, owned by renderer
// Frame-boundary quick states retain fine geometry/masks as well as canonical native VRAM/cache. Caller owns the complete 1024*512*scale*scale word image.
int metal_renderer_save_state(metal_renderer *r, uint16_t *fine, size_t words);
int metal_renderer_load_state(metal_renderer *r, const uint16_t *fine, int source_scale);
