// Phase 7 diagnostics: issue-form values are encoded by field ID, rather than placed in an unstructured issue body. No disc contents belong in a report.
#pragma once
#include <stddef.h>
typedef struct front_report_info { const char *version, *commit, *device, *game, *renderer; } front_report_info;
int front_report_url(char *out, size_t cap, const front_report_info *info);
void front_reports_start(int headless);
void front_reports_game(const char *serial);
void front_reports_menu(void *menu);   // NSMenu, on the main thread
