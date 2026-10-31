// The Mac keyboard contract (plan.md, Mac product; README, Keyboard controls): WASD + IJKL separates movement and face buttons. Shifted releases must resolve to
// the same pad bit as unshifted presses, and Mac Delete must act as Backspace/Select. This uses synthetic characters only, with no AppKit window, game data or GPU.
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
    printf("keyboard: %d checks, %d failed\n", checks, fails); return fails != 0;
}
