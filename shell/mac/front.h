// Mac front end (front_mac.m): one window, whose view is backed by the CAMetalLayer that gfx/metal presents to, and a thread for the machine. The iOS front end (phase 7)
// implements the same interface
#pragma once
#include <stdint.h>
// Opens a window titled title, runs run(arg) on a thread of its own and gives the main thread to AppKit. The app quits when run returns, on ⌘Q, or when the window closes
void front_run(const char *title, void (*run)(void *arg), void *arg);
int front_stopping(void);   // machine thread: quit requests wait for the machine to flush its memory cards and return
void front_cancel_stop(const char *message);   // a failed save keeps the machine alive and cancels the pending quit
int front_renderer(void);   // frame-boundary request: 0 software, 1/2/4 Metal scale; the machine thread owns renderer recreation
void front_renderer_set(int scale);   // any thread; ignores other values and updates the Graphics menu on the main thread
// Shows w x h BGRA8 pixels (0xAARRGGBB words) in the window: copied, then presented on the main thread. Any thread
void front_present(const uint32_t *px, int w, int h);
uint16_t front_buttons(void);   // active-high digital pad buttons, sampled on the machine thread
void front_audio(const int16_t *samples, int frames);   // 44.1 kHz signed stereo from the machine thread
// Writes w x h BGRA8 pixels to a PNG file: 0, or -1 after a [RED] line
int front_write_png(const char *path, const uint32_t *px, int w, int h);
