#ifndef APP_H
#define APP_H

#include <stdint.h>

void app_init(void);
void app_loop(void);

void app_set_price(uint16_t p);
uint16_t app_get_price(void);

#endif
