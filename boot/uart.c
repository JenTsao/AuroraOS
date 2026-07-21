#include "uart.h"

#ifdef SOC_AMBIQ_APOLLO3_BLUE

void uart_init(void) {}
void uart_putc(char c) { (void)c; }
char uart_getc(void) { return 0; }
void uart_puts(const char *s) { (void)s; }
int uart_getc_nb(char *c) { (void)c; return 0; }

#else

// 波特率与系统时钟统一取自 BSP (board.h)，更换板卡时无需改动驱动逻辑
void uart_init(void)
{
    UART0_CTL = 0;
    UART0_IBRD = BOARD_SYSCLK_FREQ / (16 * BOARD_UART_BAUDRATE);
    UART0_FBRD = 0;
    // 使能 16 字节 RX/TX FIFO (FEN=bit4)。否则 PL011 处于 1 级 FIFO 模式，
    // QEMU 经 TCP 一次性送入 "help\r\n" 而 guest 轮询被调度抢占时会发生溢出丢字节，
    // 导致 HIL 收到的命令残缺、read() 卡在终止符（之前 EXEC 不触发、或命令变 "hel"）。
    UART0_LCRH = (1 << 4) | (0x3 << 5);
    UART0_IMSC = 0;
    UART0_CTL = (1 << 0) | (1 << 8) | (1 << 9);
}

void uart_putc(char c)
{
    while (UART0_FR & UART_FR_TXFF);
    UART0_DR = c;
}

char uart_getc(void)
{
    while (UART0_FR & UART_FR_RXFE);
    return (char)(UART0_DR & 0xFF);
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

// 非阻塞读取：如果有数据返回 1 并填入字符，否则立即返回 0
int uart_getc_nb(char *c) {
    if (UART0_FR & UART_FR_RXFE) {
        return 0; // 接收 FIFO 为空，直接返回
    }
    *c = (char)(UART0_DR & 0xFF);
    return 1;
}

#endif
