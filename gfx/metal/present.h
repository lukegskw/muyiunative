// Presentation through Metal (plan.md, section 2: the software renderer's frames are shown through Metal): frames go to the CAMetalLayer the front end gives, at their own
// size, and Core Animation scales them to the layer. There is no shader yet, so nothing needs the Metal compiler
#pragma once
#include <stdint.h>
int present_init(void *layer);                              // a CAMetalLayer; 0 when there is no Metal device
void present_frame(const uint32_t *bgra, int w, int h);     // w x h BGRA8 pixels (0xAARRGGBB words) to the layer (main thread)
