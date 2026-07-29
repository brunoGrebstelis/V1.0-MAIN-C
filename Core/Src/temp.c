#include "temp.h"
#include "main.h"

extern ADC_HandleTypeDef hadc1;

typedef struct {
    uint16_t adc;
    int16_t t100;
} temp_lut_point_t;

static const temp_lut_point_t temp_lut[] = {
    {   1U,  12500},
    { 128U,  12500},
    { 256U,  10159},
    { 384U,   8660},
    { 512U,   7632},
    { 640U,   6848},
    { 768U,   6210},
    { 896U,   5668},
    {1024U,   5195},
    {1152U,   4772},
    {1280U,   4386},
    {1408U,   4029},
    {1536U,   3695},
    {1664U,   3378},
    {1792U,   3075},
    {1920U,   2783},
    {2048U,   2499},
    {2176U,   2220},
    {2304U,   1944},
    {2432U,   1668},
    {2560U,   1392},
    {2688U,   1111},
    {2816U,    824},
    {2944U,    526},
    {3072U,    215},
    {3200U,   -116},
    {3328U,   -473},
    {3456U,   -869},
    {3584U,  -1322},
    {3712U,  -1863},
    {3840U,  -2566},
    {3968U,  -3648},
    {4094U,  -4000},
};

static int16_t s_last_t100 = 2500;

static int16_t temp_adc_to_t100(uint16_t adc)
{
    const uint32_t count = (uint32_t)(sizeof(temp_lut) / sizeof(temp_lut[0]));

    if (adc <= temp_lut[0].adc) {
        return temp_lut[0].t100;
    }

    for (uint32_t i = 1U; i < count; i++) {
        if (adc <= temp_lut[i].adc) {
            const int32_t adc0 = (int32_t)temp_lut[i - 1U].adc;
            const int32_t adc1 = (int32_t)temp_lut[i].adc;
            const int32_t t0 = (int32_t)temp_lut[i - 1U].t100;
            const int32_t t1 = (int32_t)temp_lut[i].t100;
            const int32_t span_adc = adc1 - adc0;
            const int32_t span_t = t1 - t0;

            if (span_adc <= 0) {
                return (int16_t)t1;
            }

            return (int16_t)(t0 + ((span_t * ((int32_t)adc - adc0)) / span_adc));
        }
    }

    return temp_lut[count - 1U].t100;
}

int16_t temp_read_t100(void)
{
    HAL_StatusTypeDef st;
    uint32_t adc_raw;

    st = HAL_ADC_Start(&hadc1);
    if (st != HAL_OK) {
        return s_last_t100;
    }

    st = HAL_ADC_PollForConversion(&hadc1, 2U);
    if (st != HAL_OK) {
        (void)HAL_ADC_Stop(&hadc1);
        return s_last_t100;
    }

    adc_raw = HAL_ADC_GetValue(&hadc1);
    (void)HAL_ADC_Stop(&hadc1);

    if (adc_raw == 0U) {
        adc_raw = 1U;
    } else if (adc_raw >= 4095U) {
        adc_raw = 4094U;
    }

    s_last_t100 = temp_adc_to_t100((uint16_t)adc_raw);
    return s_last_t100;
}

float temp_read_c(void)
{
    return (float)temp_read_t100() / 100.0f;
}
