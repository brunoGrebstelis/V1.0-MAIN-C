#ifndef LIGHTING_MODE_H
#define LIGHTING_MODE_H

#include <stdint.h>

void lighting_mode_init(void);
void lighting_mode_set_base_color(uint8_t r, uint8_t g, uint8_t b);
void lighting_mode_set(uint8_t modeVal);
void lighting_mode_task(void);

// Kept to match your requested API name
void run_light_show(uint8_t modeVal);

#endif
