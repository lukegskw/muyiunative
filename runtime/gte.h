// COP2 geometry engine (plan.md, phase 2); psx-spx, Geometry Transformation Engine: register transfers, commands, and busy interlocks
#pragma once
#include <stdint.h>
struct ps1;
uint32_t gte_read(struct ps1 *m, unsigned reg, int control);
void gte_write(struct ps1 *m, unsigned reg, uint32_t value, int control);
void gte_command(struct ps1 *m, uint32_t op);
