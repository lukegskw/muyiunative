// Fixed keyboard layout chosen on 2026-10-31 (plan.md, decision log): WASD for the D-pad, IJKL for the face buttons. AppKit supplies charactersIgnoringModifiers;
// fold ASCII case so Shift cannot change a held letter's button. AppKit, NSText.h defines Return (0Dh), Enter (03h), Backspace (08h) and Mac Delete (7Fh).
// This is host input policy, not console state. Keeping it plain C lets t_keyboard check it without a window or GPU; digital-pad bits come from runtime/sio.h.
#pragma once
#include "../../runtime/sio.h"

static inline uint16_t front_key_button(uint32_t key) {
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    switch (key) {
    case 'w': return PAD_UP; case 'a': return PAD_LEFT; case 's': return PAD_DOWN; case 'd': return PAD_RIGHT;
    case 'i': return PAD_TRIANGLE; case 'j': return PAD_SQUARE; case 'k': return PAD_CROSS; case 'l': return PAD_CIRCLE;
    case 'q': return PAD_L1; case 'e': return PAD_R1; case '1': return PAD_L2; case '3': return PAD_R2;
    case '\r': case 0x03: return PAD_START; case '\b': case 0x7f: return PAD_SELECT;
    default: return 0;
    }
}
