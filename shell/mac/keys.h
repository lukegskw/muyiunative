// Default keyboard layout chosen on 2026-10-31 (plan.md, decision log): WASD for the D-pad, IJKL for the face buttons. AppKit supplies charactersIgnoringModifiers;
// fold ASCII case so Shift cannot change a held letter's button. AppKit, NSText.h defines Return (0Dh), Enter (03h), Backspace (08h) and Mac Delete (7Fh).
// This is host input policy, not console state. Keeping it plain C lets t_keyboard check it without a window or GPU; digital-pad bits come from runtime/sio.h.
#pragma once
#include "../../runtime/sio.h"
#include <string.h>

enum { FRONT_KEY_COUNT = 16, FRONT_KEY_CODES = 256, FRONT_KEY_BUTTONS = 0xfff9 };
typedef struct front_keys { uint32_t key[FRONT_KEY_COUNT]; uint16_t held[FRONT_KEY_CODES]; } front_keys;

static inline uint32_t front_key_normalize(uint32_t key) {
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    if (key == 0x03) key = '\r';
    if (key == '\b') key = 0x7f;
    return key;
}

static inline int front_key_valid(uint32_t key) {
    return key == 0 || key == '\r' || key == '\t' || (key >= 0x20 && key <= 0xffff && !(key >= 0xd800 && key <= 0xdfff));
}

static inline void front_keys_reset(front_keys *keys) { memset(keys->held, 0, sizeof keys->held); }
static inline void front_keys_defaults(front_keys *keys) {
    static const uint32_t defaults[FRONT_KEY_COUNT] = {0x7f, 0, 0, '\r', 'w', 'd', 's', 'a', '1', '3', 'q', 'e', 'i', 'l', 'k', 'j'};
    memcpy(keys->key, defaults, sizeof defaults); front_keys_reset(keys);
}

// One key owns one button. Reassigning it unbinds its old button; zero explicitly unbinds a button. Invalid preferences leave the previous mapping intact.
static inline int front_keys_assign(front_keys *keys, unsigned button, uint32_t key) {
    key = front_key_normalize(key);
    if (button >= FRONT_KEY_COUNT || !(FRONT_KEY_BUTTONS & (1u << button)) || !front_key_valid(key)) return -1;
    for (unsigned i = 0; i < FRONT_KEY_COUNT; i++) if (key && keys->key[i] == key) keys->key[i] = 0;
    keys->key[button] = key; front_keys_reset(keys); return 0;
}

// Load a whole preference atomically. Unlike an interactive reassignment, a duplicate here is malformed input, not a request to move its old button.
static inline int front_keys_load(front_keys *keys, const uint32_t *values, size_t n) {
    if (n != FRONT_KEY_COUNT) return -1;
    front_keys candidate = {0};
    for (unsigned i = 0; i < FRONT_KEY_COUNT; i++) {
        uint32_t key = front_key_normalize(values[i]);
        if (!front_key_valid(key) || (key && !(FRONT_KEY_BUTTONS & (1u << i)))) return -1;
        for (unsigned j = 0; key && j < i; j++) if (candidate.key[j] == key) return -1;
        candidate.key[i] = key;
    }
    *keys = candidate; return 0;
}

// Cache the button at key-down, indexed by physical keyCode: a release after Shift/layout changes must clear the original button. Repeat never changes ownership.
static inline uint16_t front_keys_event(front_keys *keys, unsigned code, uint32_t key, int down) {
    if (code < FRONT_KEY_CODES) {
        if (!down) keys->held[code] = 0;
        else if (!keys->held[code]) {
            key = front_key_normalize(key);
            for (unsigned i = 0; key && i < FRONT_KEY_COUNT; i++) if (keys->key[i] == key) keys->held[code] |= (uint16_t)(1u << i);
        }
    }
    uint16_t held = 0; for (unsigned i = 0; i < FRONT_KEY_CODES; i++) held |= keys->held[i]; return held;
}

static inline uint16_t front_key_button(uint32_t key) {
    key = front_key_normalize(key);
    switch (key) {
    case 'w': return PAD_UP; case 'a': return PAD_LEFT; case 's': return PAD_DOWN; case 'd': return PAD_RIGHT;
    case 'i': return PAD_TRIANGLE; case 'j': return PAD_SQUARE; case 'k': return PAD_CROSS; case 'l': return PAD_CIRCLE;
    case 'q': return PAD_L1; case 'e': return PAD_R1; case '1': return PAD_L2; case '3': return PAD_R2;
    case '\r': case 0x03: return PAD_START; case '\b': case 0x7f: return PAD_SELECT;
    default: return 0;
    }
}
