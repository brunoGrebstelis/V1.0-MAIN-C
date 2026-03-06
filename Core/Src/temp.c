#include "temp.h"
#include "main.h"
#include <math.h>

#define ADC_MAX 4095.0f
#define VREF    3.3f

#define R_FIXED 10000.0f
#define R0      10000.0f
#define BETA    3950.0f
#define T0      298.15f

extern ADC_HandleTypeDef hadc1;

float temp_read_c(void)
{
    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, HAL_MAX_DELAY);
    uint32_t adc_raw = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);

    if (adc_raw == 0) adc_raw = 1;
    if (adc_raw >= 4095) adc_raw = 4094;

    float v = (adc_raw / ADC_MAX) * VREF;
    float r_ntc = R_FIXED * (v / (VREF - v));

    float tempK = 1.0f / ( (1.0f / T0) + (1.0f / BETA) * logf(r_ntc / R0) );
    return tempK - 273.15f;
}
