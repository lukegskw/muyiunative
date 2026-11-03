// Ordered host rendering operations (plan.md, phase 6). The software GPU remains canonical console state; an optional per-machine consumer independently draws
// these operations in Metal. No Apple types belong in the runtime. This layout is also included by the offline Metal compiler; scalar fields keep its ABI explicit.
#pragma once
#ifdef __METAL_VERSION__
#include <metal_stdlib>
using namespace metal;
typedef uint render_u32;
typedef ushort render_u16;
#else
#include <stdint.h>
typedef uint32_t render_u32;
typedef uint16_t render_u16;
#endif

typedef struct ps1_vtx { int x, y, r, g, b, u, v; } ps1_vtx;
enum { RENDER_TRIANGLE = 1, RENDER_LINE, RENDER_RECT, RENDER_FILL, RENDER_COPY, RENDER_UPLOAD, RENDER_CLEAR, RENDER_RESET };
enum { RENDER_TEXTURE = 1, RENDER_GOURAUD = 2, RENDER_SEMI = 4, RENDER_RAW = 8, RENDER_DITHER = 16,
       RENDER_FEEDBACK = 32, RENDER_CLASSIFIED = 64 };   // host-only alias classification; the runtime emits neither bit
typedef struct ps1_render_op {
    render_u32 type, flags, texpage, texwin, mask, clut;
    int ax1, ay1, ax2, ay2;
    ps1_vtx a, b, c;
    render_u32 p[4];
} ps1_render_op;
// Byte layout matches ps1_gpu's contiguous texcache..clut_n fields, including the alignment after clut_id. Compared independently of drawing register state.
typedef struct ps1_render_cache {
    render_u16 texcache[256][4], clutcache[256], clut_id;
    render_u32 textag[256]; int clut_valid, clut_n;
} ps1_render_cache;
// p fields: RECT = width,height; FILL = rgb24,xy,wh; COPY = source_xy,dest_xy,wh; UPLOAD = VRAM word index,raw16,transfer_index.
// CLEAR p[0]=1 clears only CLUT validity (GP1 01h); zero clears texture tags and CLUT validity. RESET clears all cache contents, preserving VRAM.
