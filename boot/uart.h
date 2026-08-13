#ifndef AURORA_UART_H
#define AURORA_UART_H

#include <stdint.h>

#include "board.h"

#define UART0_DR (*(volatile uint32_t*)(BOARD_UART0_BASE + 0x000))
#define UART0_FR (*(volatile uint32_t*)(BOARD_UART0_BASE + 0x018))
#define UART0_IBRD (*(volatile uint32_t*)(BOARD_UART0_BASE + 0x024))
#define UART0_FBRD (*(volatile uint32_t*)(BOARD_UART0_BASE + 0x028))
#define UART0_LCRH (*(volatile uint32_t*)(BOARD_UART0_BASE + 0x02C))
#define UART0_CTL (*(volatile uint32_t*)(BOARD_UART0_BASE + 0x030))
#define UART0_IMSC (*(volatile uint32_t*)(BOARD_UART0_BASE + 0x038))

#define UART_FR_TXFF (1 << 5)
#define UART_FR_RXFE (1 << 4)

#ifdef __cplusplus
extern "C" {
#endif

void uart_init(void);
void uart_putc(char c);
char uart_getc(void);
void uart_puts(const char* s);
int uart_getc_nb(char* c);

#ifdef __cplusplus
}
#endif

#endif
