// Host-owned files for the console's two raw 128KB memory cards (plan.md, phase 5). Only the machine thread touches these handles or the cards.
#pragma once
struct ps1;
typedef struct front_saves { int dir_fd, lock_fd; } front_saves;
// Creates the directory, locks it against a second running copy, and loads both slots. Only absent files get newly formatted cards. Returns 0/-1 with a [RED] line.
int front_saves_open(front_saves *s, struct ps1 *m, const char *directory);
int front_saves_flush(front_saves *s, struct ps1 *m);   // atomically replace dirty cards; failure leaves the dirty flag set for a later retry
void front_saves_close(front_saves *s);               // call flush first; this only releases the directory and its lock
