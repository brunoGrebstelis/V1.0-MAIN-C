#include "app.h"
#include "display.h"
#include "rgb.h"
#include "i2c_slave.h"
#include "terminal.h"
#include "lighting_mode.h"
#include "nv_store.h"

#define APP_DEFAULT_PRICE        2026U
#define APP_DEFAULT_BASE_R       150U
#define APP_DEFAULT_BASE_G       150U
#define APP_DEFAULT_BASE_B       150U
#define APP_DEFAULT_LIGHT_MODE   0U

// Shared state updated by protocol / I2C commands
static const uint8_t g_locker_id = 5;
static volatile uint16_t g_price = APP_DEFAULT_PRICE;
static volatile uint8_t g_base_r = APP_DEFAULT_BASE_R;
static volatile uint8_t g_base_g = APP_DEFAULT_BASE_G;
static volatile uint8_t g_base_b = APP_DEFAULT_BASE_B;
static volatile uint8_t g_light_mode = APP_DEFAULT_LIGHT_MODE;

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

    // Debug: periodic I2C counters over terminal
    i2c_slave_debug_task();

    // 2) Update display value if changed
    static uint16_t last_price = 0xFFFF;
    uint16_t p = g_price;
    if (p != last_price) {
        last_price = p;
        display_set_number(p);
    }

    // 3) Run non-blocking lighting effects state machine
    lighting_mode_task();

    // 4) Persist pending state updates (outside IRQ context)
    nv_store_task();

    // 5) Optional: remove demo in production
    //rgb_demo_task();
}
