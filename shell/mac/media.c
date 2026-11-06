// A game waits for the cover switch before checking its next disc. Separate open/insert operations let guest polling observe the cover without an arbitrary timeout.
// psx-spx, CDROM Status Commands (Status code/Nop). Runtime cd_set_shell supplies the hardware notification; the frontend owns paths and error feedback.
#include "media.h"
#include "ps1.h"

int front_disc_apply(ps1 *m, int action, const char *path, char *error) {
    if (action == FRONT_DISC_OPEN) { cd_set_shell(m, 1); return 0; }
    if (action != FRONT_DISC_CLOSE && action != FRONT_DISC_INSERT) { snprintf(error, FRONT_DISC_ERROR, "Unknown disc operation."); return -1; }
    if (!m->cd.shell_open) { snprintf(error, FRONT_DISC_ERROR, "Open the disc cover before inserting a disc."); return -1; }
    if (action == FRONT_DISC_INSERT && (!path || disc_open(m, path))) {
        snprintf(error, FRONT_DISC_ERROR, "Could not open this disc. Choose a supported CUE or raw BIN with all its track files. The cover remains open."); return -1;
    }
    cd_set_shell(m, 0); return 0;
}
