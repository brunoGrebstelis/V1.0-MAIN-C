#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

void display_init(void);
void display_set_number(uint16_t number);
void display_blank(uint8_t blank);     // 1=blank, 0=normal
void display_task(void);              // call often (or from timer)

#endif
