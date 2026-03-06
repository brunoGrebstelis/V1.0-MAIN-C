#include "display.h"
#include "main.h"

// ===================== 7-seg mapping (ACTIVE-LOW segments, ACTIVE-HIGH digits) =====================
#define SEG_A_PORT GPIOA
#define SEG_A_PIN  GPIO_PIN_15

#define SEG_B_PORT GPIOA
#define SEG_B_PIN  GPIO_PIN_4

#define SEG_C_PORT GPIOC
#define SEG_C_PIN  GPIO_PIN_6

#define SEG_D_PORT GPIOB
#define SEG_D_PIN  GPIO_PIN_5

#define SEG_E_PORT GPIOB
#define SEG_E_PIN  GPIO_PIN_6

#define SEG_F_PORT GPIOA
#define SEG_F_PIN  GPIO_PIN_7

#define SEG_G_PORT GPIOB
#define SEG_G_PIN  GPIO_PIN_1

#define SEG_DP_PORT GPIOB
#define SEG_DP_PIN  GPIO_PIN_4

#define DIG1_PORT GPIOA
#define DIG1_PIN  GPIO_PIN_11

#define DIG2_PORT GPIOA
#define DIG2_PIN  GPIO_PIN_6

#define DIG3_PORT GPIOA
#define DIG3_PIN  GPIO_PIN_5

#define DIG4_PORT GPIOB
#define DIG4_PIN  GPIO_PIN_0

// segments bitmask order: A B C D E F G (LSB=A)
static const uint8_t digit_segments[] = {
    0b00111111, // 0
    0b00000110, // 1
    0b01011011, // 2
    0b01001111, // 3
    0b01100110, // 4
    0b01101101, // 5
    0b01111101, // 6
    0b00000111, // 7
    0b01111111, // 8
    0b01101111, // 9
    0b01000000, // -
    0b00000000  // empty
};

static volatile uint16_t g_number = 0;
static volatile uint8_t  g_blank  = 0;

// refresh state
static uint8_t pos = 1;
static uint32_t last_tick = 0;

// ---- low-level helpers ----
static inline void seg_write(GPIO_TypeDef *port, uint16_t pin, uint8_t on)
{
    // active-low: ON -> RESET, OFF -> SET
    if (on) port->BSRR = ((uint32_t)pin << 16);
    else    port->BSRR = (uint32_t)pin;
}

static inline void dig_write(GPIO_TypeDef *port, uint16_t pin, uint8_t on)
{
    // active-high: ON -> SET, OFF -> RESET
    if (on) port->BSRR = (uint32_t)pin;
    else    port->BSRR = ((uint32_t)pin << 16);
}

static inline void clear_segments(void)
{
    HAL_GPIO_WritePin(GPIOA, SEG_A_PIN | SEG_B_PIN | SEG_F_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, SEG_D_PIN | SEG_E_PIN | SEG_G_PIN | SEG_DP_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOC, SEG_C_PIN, GPIO_PIN_SET);
}

static inline void clear_digits(void)
{
    HAL_GPIO_WritePin(GPIOA, DIG1_PIN | DIG2_PIN | DIG3_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, DIG4_PIN, GPIO_PIN_RESET);
}

static inline void delay_us(uint32_t us)
{
    uint32_t iters = us * (SystemCoreClock / 1000000u) / 4u;
    for (volatile uint32_t i = 0; i < iters; i++) __asm volatile("nop");
}

static void display_digit(uint8_t digit, uint8_t position)
{
    if (digit > 11) return;

    clear_digits();
    delay_us(10);
    clear_segments();
    delay_us(3);

    uint8_t seg = digit_segments[digit];

    seg_write(SEG_A_PORT, SEG_A_PIN, (seg & 0x01));
    seg_write(SEG_B_PORT, SEG_B_PIN, (seg & 0x02));
    seg_write(SEG_C_PORT, SEG_C_PIN, (seg & 0x04));
    seg_write(SEG_D_PORT, SEG_D_PIN, (seg & 0x08));
    seg_write(SEG_E_PORT, SEG_E_PIN, (seg & 0x10));
    seg_write(SEG_F_PORT, SEG_F_PIN, (seg & 0x20));
    seg_write(SEG_G_PORT, SEG_G_PIN, (seg & 0x40));

    // DP off
    HAL_GPIO_WritePin(SEG_DP_PORT, SEG_DP_PIN, GPIO_PIN_SET);

    delay_us(3);

    switch (position) {
        case 1: dig_write(DIG1_PORT, DIG1_PIN, 1); break;
        case 2: dig_write(DIG2_PORT, DIG2_PIN, 1); break;
        case 3: dig_write(DIG3_PORT, DIG3_PIN, 1); break;
        case 4: dig_write(DIG4_PORT, DIG4_PIN, 1); break;
        default: break;
    }
}

void display_init(void)
{
    clear_digits();
    clear_segments();
    g_number = 0;
    g_blank = 0;
}

void display_set_number(uint16_t number)
{
    if (number > 9999) number = 9999;
    g_number = number;
}

void display_blank(uint8_t blank)
{
    g_blank = blank ? 1 : 0;
    if (g_blank) {
        clear_digits();
        clear_segments();
    }
}

// call this often; it updates ONE digit each time
void display_task(void)
{
    if (g_blank) return;

    // refresh rate control
    // 1ms per digit => ~250Hz full frame (4 digits)
    uint32_t now = HAL_GetTick();
    if ((now - last_tick) < 1) return;
    last_tick = now;

    uint16_t n = g_number;

    // break into digits
    uint8_t d1 =  n / 1000;
    uint8_t d2 = (n / 100) % 10;
    uint8_t d3 = (n / 10)  % 10;
    uint8_t d4 =  n % 10;

    // leading blanking
    uint8_t dig_to_show = 11; // empty
    if (pos == 1) dig_to_show = (n >= 1000) ? d1 : 11;
    if (pos == 2) dig_to_show = (n >= 100)  ? d2 : 11;
    if (pos == 3) dig_to_show = (n >= 10)   ? d3 : 11;
    if (pos == 4) dig_to_show = d4;

    display_digit(dig_to_show, pos);

    pos++;
    if (pos > 4) pos = 1;
}
