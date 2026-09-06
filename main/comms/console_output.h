#pragma once

/* Single console owner: complete JSONL records are emitted with one stdio
 * write. Other tasks may log normally while a response is being assembled. */
#ifdef __GNUC__
__attribute__((format(printf, 1, 2)))
#endif
int d1l_console_printf(const char *format, ...);
int d1l_console_putchar(int character);
