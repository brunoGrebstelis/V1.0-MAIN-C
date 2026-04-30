#ifndef APP_H
#define APP_H

#include <stdint.h>

void app_init(void);
void app_loop(void);

uint8_t app_get_locker_id(void);
void app_set_price(uint16_t p);
void app_set_rgb(uint8_t r, uint8_t g, uint8_t b);
void app_set_light_mode(uint8_t modeVal);
uint16_t app_get_price(void);

#endif
