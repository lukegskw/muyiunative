// Presentation through Metal (plan.md, section 2: the software renderer's frames are shown through Metal): frames go to the CAMetalLayer the front end gives.
// Phase 0 clears it to black; phase 1 draws the software GPU's VRAM. There is no shader yet, so nothing needs the Metal compiler
#pragma once
int present_init(void *layer);   // a CAMetalLayer; 0 when there is no Metal device
void present_frame(void);        // the next frame to the layer (main thread)
