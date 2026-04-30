#include "app.h"
#include "display.h"
#include "rgb.h"
#include "i2c_slave.h"
#include "terminal.h"
#include "temp.h"

#include "main.h"   // CubeMX handles live here

// Shared state updated by protocol
static const uint8_t g_locker_id = 5;
static volatile uint16_t g_price = 2026;

uint8_t app_get_locker_id(void) { return g_locker_id; }
uint16_t app_get_price(void) { return g_price; }
void app_set_price(uint16_t p) { g_price = p; }

void app_init(void)
{
    display_init();
    rgb_init();
    terminal_init();

    // Start I2C slave reception (command-based protocol)
    i2c_slave_init();

    // Startup announce frame: [CMD=0x02, LOCKER_ID, DATA_H, DATA_L]
    // Delay = 50 ms * locker ID (decimal value)
    HAL_Delay(50U * (uint32_t)g_locker_id);
    {
        uint8_t startup_frame[4] = {
            0x02U,
            g_locker_id,
            (uint8_t)(g_price >> 8),
            (uint8_t)(g_price & 0xFF)
        };
        i2c_slave_send_reply(startup_frame, sizeof(startup_frame));
    }

    // Example startup state
    display_set_number(g_price);
    rgb_set(150, 150, 150);
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

    // 3) Optional: remove demo in production
    //rgb_demo_task();
}
