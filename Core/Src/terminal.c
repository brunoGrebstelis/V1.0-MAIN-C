#include "terminal.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

extern UART_HandleTypeDef huart2;

void terminal_init(void)
{
    // nothing required; UART init is CubeMX
}

void terminal_print(const char *s)
{
    if (!s) return;
    HAL_UART_Transmit(&huart2, (uint8_t*)s, (uint16_t)strlen(s), 100);
}

void terminal_println(const char *s)
{
    terminal_print(s);
    terminal_print("\r\n");
}

void terminal_printf(const char *fmt, ...)
{
    char buf[128];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    terminal_print(buf);
}
