// Mac product keyboard preferences (plan.md, phase 5). Main-thread only; the front end publishes the returned mask atomically to the machine thread.
#pragma once
#include <stdint.h>
void front_keyboard_load(void);
void front_keyboard_settings(void);
uint16_t front_keyboard_event(uint16_t keycode, uint32_t character, int down);
void front_keyboard_reset(void);
