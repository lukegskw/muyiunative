// Phase 7's controller policy, chosen on 2026-11-02 (plan.md, decision log): face buttons by position, digital triggers and left-stick directions at 0.5.
// front_mac.m supplies a captured GameController profile; this plain C state owns port assignment, mapping and focus, never a machine. PS1 digital button bits
// are from psx-spx, Controllers and Memory Cards, Standard Controllers. t_controllers checks the policy without Apple frameworks, devices, discs or a GPU.
#pragma once
#include "front.h"
#include "../../runtime/sio.h"

typedef struct front_controller_input {
    int south, east, west, north, l1, r1, menu, options, up, right, down, left;
    float l2, r2, x, y;
} front_controller_input;
typedef struct front_controllers { uintptr_t id[2]; uint16_t held[2]; int focused; } front_controllers;

static inline uint16_t front_controller_buttons(front_controller_input in) {
    uint16_t b = 0;
    if (in.south) b |= PAD_CROSS; if (in.east) b |= PAD_CIRCLE;
    if (in.west) b |= PAD_SQUARE; if (in.north) b |= PAD_TRIANGLE;
    if (in.l1) b |= PAD_L1; if (in.r1) b |= PAD_R1;
    if (in.menu) b |= PAD_START; if (in.options) b |= PAD_SELECT;
    if (in.l2 >= 0.5f) b |= PAD_L2; if (in.r2 >= 0.5f) b |= PAD_R2;
    if (in.up || in.y >= 0.5f) b |= PAD_UP; if (in.down || in.y <= -0.5f) b |= PAD_DOWN;
    if (in.right || in.x >= 0.5f) b |= PAD_RIGHT; if (in.left || in.x <= -0.5f) b |= PAD_LEFT;
    return b;
}

// Opaque nonzero identities belong to retained host devices. Keep a surviving controller on its port when another disconnects; a newcomer fills the first vacancy.
static inline int front_controller_connect(front_controllers *s, uintptr_t id) {
    if (!id) return -1;
    for (int i = 0; i < 2; i++) if (s->id[i] == id) return i;
    for (int i = 0; i < 2; i++) if (!s->id[i]) { s->id[i] = id; s->held[i] = 0; return i; }
    return -1;
}
static inline void front_controller_disconnect(front_controllers *s, uintptr_t id) {
    for (int i = 0; i < 2; i++) if (s->id[i] == id) { s->id[i] = 0; s->held[i] = 0; }
}
static inline void front_controllers_focus(front_controllers *s, int focused) {
    s->focused = focused; s->held[0] = s->held[1] = 0;
}
static inline void front_controller_sample(front_controllers *s, uintptr_t id, front_controller_input in) {
    for (int i = 0; i < 2; i++) if (id && s->id[i] == id) s->held[i] = s->focused ? front_controller_buttons(in) : 0;
}
static inline void front_controllers_read(const front_controllers *s, uint16_t keyboard, front_pad pads[2]) {
    pads[0] = (front_pad){1, s->focused ? (uint16_t)(keyboard | s->held[0]) : 0};
    pads[1] = (front_pad){s->id[1] != 0, s->focused ? s->held[1] : 0};
}
