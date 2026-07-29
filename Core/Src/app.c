#include "app.h"
#include "main.h"
#include "display.h"
#include "rgb.h"
#include "i2c_slave.h"
#include "terminal.h"
#include "lighting_mode.h"
#include "nv_store.h"
#include "temp.h"

extern IWDG_HandleTypeDef hiwdg;

#define APP_DEFAULT_PRICE        2026U
#define APP_DEFAULT_BASE_R       150U
#define APP_DEFAULT_BASE_G       150U
#define APP_DEFAULT_BASE_B       150U
#define APP_DEFAULT_LIGHT_MODE   0U
#define APP_WDG_REFRESH_PERIOD_MS 1200U
#define APP_TEMP_SAMPLE_PERIOD_MS 1000U
#define APP_TEMP_AVG_SAMPLE_COUNT 30U

// Shared state updated by protocol / I2C commands
static const uint8_t g_locker_id = 10;
static volatile uint16_t g_price = APP_DEFAULT_PRICE;
static volatile uint8_t g_base_r = APP_DEFAULT_BASE_R;
static volatile uint8_t g_base_g = APP_DEFAULT_BASE_G;
static volatile uint8_t g_base_b = APP_DEFAULT_BASE_B;
static volatile uint8_t g_light_mode = APP_DEFAULT_LIGHT_MODE;
static volatile int16_t g_temp100 = 2500;
static int16_t g_temp_samples[APP_TEMP_AVG_SAMPLE_COUNT] = {0};
static int32_t g_temp_sample_sum = 0;
static uint8_t g_temp_sample_index = 0;
static uint8_t g_temp_sample_count = 0;

static int16_t app_temp_to_t100(float temp_c)
{
    int32_t t100 = (int32_t)(temp_c * 100.0f);

    if (t100 < -4000L) {
        t100 = -4000L;
    } else if (t100 > 12500L) {
        t100 = 12500L;
    }

    return (int16_t)t100;
}

static void app_request_persist(void)
{
    nv_store_state_t st = {
        g_price,
        g_base_r,
        g_base_g,
        g_base_b,
        g_light_mode
    };

    nv_store_request_save(&st);
}

uint8_t app_get_locker_id(void) { return g_locker_id; }
uint16_t app_get_price(void) { return g_price; }
int16_t app_get_temp100(void) { return g_temp100; }

static void app_temp_task(void)
{
    static uint32_t s_last_sample_ms = 0U;
    static uint8_t s_has_sample = 0U;
    const uint32_t now_ms = HAL_GetTick();
    int16_t sample_t100;

    if (s_has_sample && ((now_ms - s_last_sample_ms) < APP_TEMP_SAMPLE_PERIOD_MS)) {
        return;
    }

    s_has_sample = 1U;
    s_last_sample_ms = now_ms;
    sample_t100 = app_temp_to_t100(temp_read_c());

    if (g_temp_sample_count < APP_TEMP_AVG_SAMPLE_COUNT) {
        g_temp_samples[g_temp_sample_index] = sample_t100;
        g_temp_sample_sum += sample_t100;
        g_temp_sample_count++;
    } else {
        g_temp_sample_sum -= g_temp_samples[g_temp_sample_index];
        g_temp_samples[g_temp_sample_index] = sample_t100;
        g_temp_sample_sum += sample_t100;
    }

    g_temp_sample_index++;
    if (g_temp_sample_index >= APP_TEMP_AVG_SAMPLE_COUNT) {
        g_temp_sample_index = 0U;
    }

    if (g_temp_sample_count > 0U) {
        g_temp100 = (int16_t)(g_temp_sample_sum / (int32_t)g_temp_sample_count);
    }
}

void app_set_price(uint16_t p)
{
    if (g_price != p) {
        g_price = p;
        app_request_persist();
    }

    display_set_number(p);
}

void app_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (g_base_r != r || g_base_g != g || g_base_b != b) {
        g_base_r = r;
        g_base_g = g;
        g_base_b = b;
        app_request_persist();
    }

    rgb_set(r, g, b);
    lighting_mode_set_base_color(r, g, b);
}

void app_set_light_mode(uint8_t modeVal)
{
    run_light_show(modeVal);

    // mode 0xFF is temporary flash mode: do not persist it
    if (modeVal != 0xFFU && g_light_mode != modeVal) {
        g_light_mode = modeVal;
        app_request_persist();
    }
}

void app_init(void)
{
    display_init();
    rgb_init();
    lighting_mode_init();
    terminal_init();

    nv_store_init();

    const nv_store_state_t defaults = {
        APP_DEFAULT_PRICE,
        APP_DEFAULT_BASE_R,
        APP_DEFAULT_BASE_G,
        APP_DEFAULT_BASE_B,
        APP_DEFAULT_LIGHT_MODE
    };
    nv_store_state_t loaded = defaults;
    nv_store_load(&loaded, &defaults);

    g_price = loaded.price;
    g_base_r = loaded.r;
    g_base_g = loaded.g;
    g_base_b = loaded.b;
    g_light_mode = loaded.mode;

    display_set_number(g_price);
    lighting_mode_set_base_color(g_base_r, g_base_g, g_base_b);
    if (g_light_mode != 0U) {
        run_light_show(g_light_mode);
    }

    // Start I2C slave reception (command-based protocol)
    i2c_slave_init();

    terminal_println("APP init OK");
}

void app_loop(void)
{
    // 1) Keep display refreshing (non-blocking multiplex)
    display_task();

    // 2) Update display value if changed
    static uint16_t last_price = 0xFFFF;
    uint16_t p = g_price;
    if (p != last_price) {
        last_price = p;
        display_set_number(p);
    }

    // 3) Run non-blocking lighting effects state machine
    lighting_mode_task();

    // 4) Keep cached temperature ready for fast I2C replies
    app_temp_task();

    // 5) Persist pending state updates (outside IRQ context)
    nv_store_task();

    // 6) Keep I2C slave resilient to malformed traffic / bus glitches
    i2c_slave_watchdog_task();

    // 7) Centralized I2C error handling/reporting (non-IRQ context)
    {
        const uint32_t i2c_err = i2c_slave_take_error_flags();
        if (i2c_err != 0U) {
            terminal_printf("I2C error flags: 0x%08lX\r\n", (unsigned long)i2c_err);
        }
    }

    // 8) Optional: remove demo in production
    //rgb_demo_task();

    // 9) Feed independent watchdog (IWDG)
    // Refresh periodically so this is safe even with window mode enabled.
    // If app_loop() stalls longer than watchdog timeout, MCU will reset.
    {
        static uint32_t s_last_wdg_kick_ms = 0U;
        const uint32_t now_ms = HAL_GetTick();

        if ((now_ms - s_last_wdg_kick_ms) >= APP_WDG_REFRESH_PERIOD_MS) {
            (void)HAL_IWDG_Refresh(&hiwdg);
            s_last_wdg_kick_ms = now_ms;
        }
    }
}
