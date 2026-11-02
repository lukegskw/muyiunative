// Phase 7's host controller contract: positional mapping, inclusive 0.5 thresholds, two stable ports, keyboard union, hotplug and focus release. Inputs are plain
// C snapshots of synthetic profiles; no GameController/AppKit, real device, game, BIOS or GPU. PS1 bit definitions: psx-spx, Controllers and Memory Cards.
#include "../controllers.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

int main(void) {
    const struct { size_t offset; uint16_t button; } buttons[] = {
        {offsetof(front_controller_input, south), PAD_CROSS}, {offsetof(front_controller_input, east), PAD_CIRCLE},
        {offsetof(front_controller_input, west), PAD_SQUARE}, {offsetof(front_controller_input, north), PAD_TRIANGLE},
        {offsetof(front_controller_input, l1), PAD_L1}, {offsetof(front_controller_input, r1), PAD_R1},
        {offsetof(front_controller_input, menu), PAD_START}, {offsetof(front_controller_input, options), PAD_SELECT},
        {offsetof(front_controller_input, up), PAD_UP}, {offsetof(front_controller_input, right), PAD_RIGHT},
        {offsetof(front_controller_input, down), PAD_DOWN}, {offsetof(front_controller_input, left), PAD_LEFT}
    };
    CHECK(!front_controller_buttons((front_controller_input){0}), "a neutral profile releases all buttons");
    for (unsigned i = 0; i < sizeof buttons / sizeof *buttons; i++) {
        front_controller_input in = {0}; *(int *)((char *)&in + buttons[i].offset) = 1;
        CHECK(front_controller_buttons(in) == buttons[i].button, "each physical button maps to exactly its documented PS1 bit");
    }
    const struct { front_controller_input in; uint16_t buttons; } analog[] = {
        {{.x = 0.499f, .y = -0.499f, .l2 = 0.499f, .r2 = 0.499f}, 0},
        {{.x = -0.499f, .y = 0.499f}, 0},
        {{.x = 0.5f}, PAD_RIGHT}, {{.x = -0.5f}, PAD_LEFT}, {{.y = 0.5f}, PAD_UP}, {{.y = -0.5f}, PAD_DOWN},
        {{.l2 = 0.5f}, PAD_L2}, {{.r2 = 0.5f}, PAD_R2}, {{.x = 1, .y = -1, .l2 = 1, .r2 = 1}, PAD_RIGHT | PAD_DOWN | PAD_L2 | PAD_R2},
        {{.up = 1, .left = 1, .x = 1, .y = 1, .south = 1}, PAD_UP | PAD_LEFT | PAD_RIGHT | PAD_CROSS},
        {{.down = 1, .x = -1, .y = -1, .menu = 1, .options = 1}, PAD_DOWN | PAD_LEFT | PAD_START | PAD_SELECT}
    };
    for (unsigned i = 0; i < sizeof analog / sizeof *analog; i++)
        CHECK(front_controller_buttons(analog[i].in) == analog[i].buttons, "stick, triggers and D-pad combine at the inclusive threshold");

    front_controllers s = {0}, other = {0}; front_pad pads[2];
    front_controllers_focus(&s, 1); front_controllers_read(&s, PAD_START, pads);
    CHECK(pads[0].connected && pads[0].buttons == PAD_START && !pads[1].connected && !pads[1].buttons, "keyboard alone connects port 1, leaving port 2 empty");
    CHECK(front_controller_connect(&s, 0) == -1, "zero is never a device identity");
    CHECK(front_controller_connect(&s, 101) == 0 && front_controller_connect(&s, 202) == 1, "the first two controllers fill ports 1 and 2");
    front_controller_input cross = {.south = 1}, square = {.west = 1};
    front_controller_sample(&s, 101, cross); front_controller_sample(&s, 202, square);
    front_controllers_read(&s, PAD_START | PAD_CROSS, pads);
    CHECK(pads[0].connected && pads[0].buttons == (PAD_START | PAD_CROSS) && pads[1].connected && pads[1].buttons == PAD_SQUARE, "keyboard unions with controller 1 without reaching controller 2");
    front_controllers_read(&s, 0, pads);
    CHECK(pads[0].buttons == PAD_CROSS && pads[1].buttons == PAD_SQUARE, "releasing keyboard input preserves the controller's same held button");
    CHECK(front_controller_connect(&s, 101) == 0 && s.held[0] == PAD_CROSS && front_controller_connect(&s, 303) == -1, "repeat notification preserves held state, and a third controller cannot replace a player");
    front_controllers old = s;
    front_controller_sample(&s, 303, cross); front_controller_disconnect(&s, 404);
    CHECK(!memcmp(&s, &old, sizeof s), "unassigned devices cannot change either port");
    front_controller_sample(&s, 101, (front_controller_input){0}); front_controllers_read(&s, PAD_CROSS, pads);
    CHECK(pads[0].buttons == PAD_CROSS, "controller release preserves a keyboard press on the same button");
    front_controller_sample(&s, 101, cross);
    front_controllers_focus(&s, 0); front_controllers_read(&s, PAD_START, pads);
    CHECK(pads[0].connected && pads[1].connected && !pads[0].buttons && !pads[1].buttons && !s.held[0] && !s.held[1], "focus loss releases both ports without unplugging them");
    front_controller_sample(&s, 101, cross); front_controller_sample(&s, 202, square); front_controllers_read(&s, PAD_START, pads);
    CHECK(!pads[0].buttons && !pads[1].buttons, "polling or keyboard events while unfocused cannot restore held inputs");
    front_controllers_focus(&s, 1); front_controllers_read(&s, 0, pads);
    CHECK(!pads[0].buttons && !pads[1].buttons, "regaining focus does not replay stale samples");
    front_controller_sample(&s, 101, cross); front_controller_sample(&s, 202, square); front_controllers_read(&s, 0, pads);
    CHECK(pads[0].buttons == PAD_CROSS && pads[1].buttons == PAD_SQUARE, "a fresh focused sample restores current physical inputs");
    front_controller_disconnect(&s, 101); front_controllers_read(&s, PAD_START, pads);
    CHECK(pads[0].connected && pads[0].buttons == PAD_START && pads[1].connected && pads[1].buttons == PAD_SQUARE && s.id[1] == 202, "unplugging player 1 releases its buttons, preserves keyboard and keeps player 2 on its port");
    CHECK(front_controller_connect(&s, 303) == 0 && !s.held[0], "a waiting controller fills the vacancy with no inherited buttons");
    front_controller_sample(&s, 101, cross); front_controllers_read(&s, 0, pads);
    CHECK(!pads[0].buttons && pads[1].buttons == PAD_SQUARE, "a stale sample after disconnect cannot affect a new device");
    front_controller_disconnect(&s, 202); front_controllers_read(&s, 0, pads);
    CHECK(!pads[1].connected && !pads[1].buttons, "unplugging player 2 disconnects its pad and releases all buttons");
    CHECK(front_controller_connect(&s, 101) == 1 && !s.held[1], "reconnecting fills the empty second port without moving player 1");
    CHECK(front_controller_connect(&other, 101) == 0, "another host may use the same opaque device identities");
    front_controller_sample(&other, 101, cross); front_controllers_read(&other, PAD_START, pads);
    CHECK(!other.held[0] && !pads[0].buttons && !pads[1].connected, "connecting and sampling while unfocused cannot activate a pad");
    old = s; front_controllers_focus(&other, 1); front_controller_sample(&other, 101, square);
    CHECK(other.held[0] == PAD_SQUARE && !memcmp(&s, &old, sizeof s), "independent host states cannot leak ports, buttons or focus");
    printf("controllers: %d checks, %d failed\n", checks, fails); return fails != 0;
}
