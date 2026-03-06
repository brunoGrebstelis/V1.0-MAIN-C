#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdarg.h>

void terminal_init(void);
void terminal_print(const char *s);
void terminal_println(const char *s);
void terminal_printf(const char *fmt, ...);

#endif
