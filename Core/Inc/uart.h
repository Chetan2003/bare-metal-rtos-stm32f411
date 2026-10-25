#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(uint32_t baud);          /* USART2 on PA2/PA3 (ST-LINK VCP) */
void uart_puts(const char *s);
void uart_put_u32(uint32_t v);

#endif
