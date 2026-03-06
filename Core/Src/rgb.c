#include "rgb.h"
#include "main.h"

// Blue pin (software PWM)
#define BLUE_PORT GPIOA
#define BLUE_PIN  GPIO_PIN_12

static volatile uint8_t blue_pwm = 0;   // 0..255
static volatile uint8_t bam_bit  = 0;

#define BASE_UNIT_TICKS 20  // same as you had

extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim14;

static void bluepwm_start(void)
{
    bam_bit = 0;
    __HAL_TIM_SET_AUTORELOAD(&htim14, (1u << bam_bit) * BASE_UNIT_TICKS);
    __HAL_TIM_SET_COUNTER(&htim14, 0);
    HAL_TIM_Base_Start_IT(&htim14);
}

void rgb_init(void)
{
    // TIM1 HW PWM for R/G must be started
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);

    bluepwm_start();

}

void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, r);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, g);
    blue_pwm = b;
}

void rgb_tim_period_elapsed_callback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM14) return;

    if (blue_pwm & (1u << bam_bit)) HAL_GPIO_WritePin(BLUE_PORT, BLUE_PIN, GPIO_PIN_SET);
    else                            HAL_GPIO_WritePin(BLUE_PORT, BLUE_PIN, GPIO_PIN_RESET);

    bam_bit++;
    if (bam_bit >= 8) bam_bit = 0;

    __HAL_TIM_SET_AUTORELOAD(&htim14, (1u << bam_bit) * BASE_UNIT_TICKS);
    __HAL_TIM_SET_COUNTER(&htim14, 0);
}

void rgb_demo_task(void)
{
    static uint32_t last = 0;
    static uint8_t step = 0;

    uint32_t now = HAL_GetTick();
    if ((now - last) < 500) return;
    last = now;

    switch (step) {
        case 0: rgb_set(255,   0,   0); break;
        case 1: rgb_set(  0, 255,   0); break;
        case 2: rgb_set(  0,   0, 255); break;
        case 3: rgb_set(255, 255, 255); break;
        default: rgb_set(128,  0, 128); break;
    }
    step = (step + 1) % 5;
}
