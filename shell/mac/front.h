// Mac front end (front_mac.m): one window, whose view is backed by the CAMetalLayer that gfx/metal presents to. The iOS front end (phase 7) implements the same interface
#pragma once
// Opens a window titled title and gives the main thread to AppKit. AppKit ends the process when the app quits (⌘Q, or the window closed)
void front_run(const char *title);
