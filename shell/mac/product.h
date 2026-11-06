// Mac product metadata and launcher (plan.md, phase 5). Framework work stays here; main.c and the runtime use plain C interfaces.
#pragma once
#include <limits.h>
#include <stddef.h>
enum { FRONT_DISC_MAX = 16 };
typedef struct front_product {
    char title[256], disc[PATH_MAX], save_id[128], serial[32]; int render_scale, ndiscs;
    char discs[FRONT_DISC_MAX][PATH_MAX], disc_titles[FRONT_DISC_MAX][256];
} front_product;
int front_product_load(front_product *p);   // 1: bundled game, 0: developer host or launcher, -1: invalid game metadata
int front_product_read(front_product *p, const char *resources);   // Foundation-only metadata loader, also exercised by synthetic product tests
int front_save_directory(char *out, size_t cap, const char *id);
void front_launcher(void);
void front_error(const char *message);
