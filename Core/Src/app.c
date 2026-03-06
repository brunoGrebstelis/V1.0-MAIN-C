#include "app.h"
#include "display.h"
#include "rgb.h"
#include "i2c_slave.h"
#include "terminal.h"
#include "temp.h"

#include "main.h"   // CubeMX handles live here

// Shared state updated by protocol
static volatile uint16_t g_price = 2026;

uint16_t app_get_price(void) { return g_price; }
void app_set_price(uint16_t p) { g_price = p; }

void app_init(void)
{
    display_init();
    rgb_init();
    terminal_init();

    // Start I2C slave reception (command-based protocol)
    i2c_slave_init();

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
