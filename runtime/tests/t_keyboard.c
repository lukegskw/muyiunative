// The Mac keyboard contract (plan.md, Mac product; README, Keyboard controls): WASD + IJKL separates movement and face buttons. Shifted releases must resolve to
// the same pad bit as unshifted presses, and Mac Delete must act as Backspace/Select. Remapping must reject malformed preferences and release the original button
// after a layout change. This uses synthetic characters only, with no AppKit window, game data or GPU.
#include "../../shell/mac/keys.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

int main(void) {
    const struct { uint32_t key; uint16_t button; } bindings[] = {
        {'w', PAD_UP}, {'a', PAD_LEFT}, {'s', PAD_DOWN}, {'d', PAD_RIGHT},
        {'i', PAD_TRIANGLE}, {'j', PAD_SQUARE}, {'k', PAD_CROSS}, {'l', PAD_CIRCLE},
        {'q', PAD_L1}, {'e', PAD_R1}, {'1', PAD_L2}, {'3', PAD_R2},
        {'\r', PAD_START}, {0x03, PAD_START}, {'\b', PAD_SELECT}, {0x7f, PAD_SELECT}
    };
    for (unsigned i = 0; i < sizeof bindings / sizeof *bindings; i++) {
        CHECK(front_key_button(bindings[i].key) == bindings[i].button, "the documented key maps to exactly its pad bit");
        if (bindings[i].key >= 'a' && bindings[i].key <= 'z')
            CHECK(front_key_button(bindings[i].key - ('a' - 'A')) == bindings[i].button, "Shift preserves a letter's pad bit");
    }
    uint16_t held = front_key_button('w') | front_key_button('j') | front_key_button('k');
    CHECK(held == (PAD_UP | PAD_SQUARE | PAD_CROSS), "movement and two face buttons combine independently");
    held &= ~front_key_button('W');
    CHECK(held == (PAD_SQUARE | PAD_CROSS), "a shifted release clears only the held direction");
    const uint32_t unmapped[] = {0, 'z', 'x', '2', ' ', 0xf700, 0xf701, 0xf702, 0xf703, 0xf728};
    for (unsigned i = 0; i < sizeof unmapped / sizeof *unmapped; i++)
        CHECK(!front_key_button(unmapped[i]), "legacy keys and forward Delete do not activate unrelated buttons");

    front_keys keys; front_keys_defaults(&keys);
    for (unsigned i = 0; i < FRONT_KEY_COUNT; i++) if (keys.key[i])
        CHECK(front_key_button(keys.key[i]) == (1u << i), "configurable defaults match the documented fixed mapping");
    CHECK(front_keys_event(&keys, 1, 'W', 1) == PAD_UP, "press caches the button for a physical key");
    CHECK(front_keys_event(&keys, 1, 'j', 1) == PAD_UP, "repeat cannot change a held key's ownership");
    CHECK(front_keys_event(&keys, 2, 'j', 1) == (PAD_UP | PAD_SQUARE), "movement and action remain held independently");
    CHECK(front_keys_event(&keys, 1, 'z', 0) == PAD_SQUARE, "release clears the original button after a layout change");
    CHECK(front_keys_event(&keys, 3, 'J', 1) == PAD_SQUARE, "two physical keys may resolve to one logical key");
    CHECK(front_keys_event(&keys, 2, 'j', 0) == PAD_SQUARE, "releasing one physical key preserves the other");
    CHECK(front_keys_event(&keys, FRONT_KEY_CODES, 'l', 1) == PAD_SQUARE, "out-of-range key codes cannot corrupt held keys");
    front_keys_reset(&keys); CHECK(front_keys_event(&keys, 0, 0, 0) == 0, "losing focus releases every held key");
    CHECK(!front_keys_assign(&keys, 14, 'J') && keys.key[14] == 'j' && !keys.key[15], "duplicate assignment moves a key to its new button");
    CHECK(front_keys_event(&keys, 4, 'j', 1) == PAD_CROSS, "remapped key uses its new button");
    CHECK(!front_keys_assign(&keys, 14, 0) && !keys.key[14] && !front_keys_event(&keys, 4, 'j', 1), "unassignment also clears held keys");
    front_keys old = keys;
    CHECK(front_keys_assign(&keys, 1, 'x') == -1 && !memcmp(&old, &keys, sizeof keys), "unused pad bits reject assignments without modifying the mapping");
    CHECK(front_keys_assign(&keys, 16, 'x') == -1 && front_keys_assign(&keys, 14, 0x10000) == -1 &&
          front_keys_assign(&keys, 14, 0xd800) == -1 && front_keys_assign(&keys, 14, 27) == -1, "invalid button indexes, surrogate keys and Escape are rejected");
    CHECK(!front_keys_assign(&keys, 4, 0xf700) && front_keys_event(&keys, 5, 0xf700, 1) == PAD_UP, "arrow keys can be assigned");
    CHECK(!front_keys_assign(&keys, 0, '\b') && keys.key[0] == 0x7f, "Backspace aliases have one canonical preference");
    CHECK(!front_keys_assign(&keys, 3, 0x03) && keys.key[3] == '\r', "Return and keypad Enter share their binding");
    front_keys_defaults(&keys);
    CHECK(keys.key[14] == 'k' && keys.key[15] == 'j' && keys.key[4] == 'w' && !front_keys_event(&keys, 0, 0, 0), "restore defaults recovers the layout and releases held keys");
    uint32_t saved[FRONT_KEY_COUNT]; memcpy(saved, keys.key, sizeof saved); saved[14] = 'Z';
    CHECK(!front_keys_load(&keys, saved, FRONT_KEY_COUNT) && keys.key[14] == 'z', "preferences load and normalize a complete custom mapping");
    old = keys; saved[15] = 'z';
    CHECK(front_keys_load(&keys, saved, FRONT_KEY_COUNT) == -1 && !memcmp(&old, &keys, sizeof keys), "duplicate preferences leave the current mapping intact");
    saved[15] = 'j'; saved[1] = 'x';
    CHECK(front_keys_load(&keys, saved, FRONT_KEY_COUNT) == -1 && !memcmp(&old, &keys, sizeof keys), "preferences cannot bind unused pad bits");
    saved[1] = 0; saved[14] = 0xd800;
    CHECK(front_keys_load(&keys, saved, FRONT_KEY_COUNT) == -1 && !memcmp(&old, &keys, sizeof keys), "malformed preference keys leave the current mapping intact");
    CHECK(front_keys_load(&keys, saved, FRONT_KEY_COUNT - 1) == -1 && !memcmp(&old, &keys, sizeof keys), "truncated preferences leave the current mapping intact");
    printf("keyboard: %d checks, %d failed\n", checks, fails); return fails != 0;
}
