// Launcher build flow (plan.md, phase 7); AppKit owns these calls. Builds run in a child process, never on the machine thread.
#pragma once
void front_build_game_app(void);
int front_build_terminate(void);   // 0: no active build; 1: confirmation/cleanup pending, reply to termination later
