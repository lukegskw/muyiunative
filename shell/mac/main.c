// The Mac host (plan.md, phase 0): makes the machine and gives the main thread to the front end. Nothing runs in the machine yet: phase 1 adds the interpreter and boots OpenBIOS
#include "front.h"
#include "ps1.h"

int main(void) {
    ps1 *m = ps1_new();
    if (!m) { RED("host: no memory for the machine"); return 1; }
    LOG("host: machine with %d KB of RAM, %d bytes of scratchpad and %d KB of BIOS ROM", RAM_SIZE >> 10, SCRATCH_SIZE, BIOS_SIZE >> 10);
    front_run("ps1native");
    ps1_free(m);
    return 0;
}
