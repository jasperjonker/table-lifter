#ifndef ROTATION_CONTROL_H_
#define ROTATION_CONTROL_H_
#include "table_lifter.h"

/* Temporary RAM-only presets, in 1/16 microsteps (3200 per shaft revolution). */
void rotation_init(void);
void rotation_tick(uint32_t now, uint8_t buttons, uint8_t limits, bool pd_ok);
bool rotation_homed(void);
int32_t rotation_position(void);
#endif
