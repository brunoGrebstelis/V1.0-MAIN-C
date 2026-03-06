#ifndef RGB_H
#define RGB_H

#include <stdint.h>
#include "main.h"

void rgb_init(void);
void rgb_set(uint8_t r, uint8_t g, uint8_t b);
void rgb_demo_task(void);

// call from HAL_TIM_PeriodElapsedCallback
void rgb_tim_period_elapsed_callback(TIM_HandleTypeDef *htim);

#endif
