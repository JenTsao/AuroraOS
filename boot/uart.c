#include "uart.h"

#ifdef SOC_AMBIQ_APOLLO3_BLUE

void uart_init(void) {}

void uart_putc(char c) {
    (void)c;
}

char uart_getc(void) {
    return 0;
}

void uart_puts(const char* s) {
    (void)s;
}

int uart_getc_nb(char* c) {
    (void)c;
    return 0;
}

#elif defined(ARCH_RISCV32)

// NS16550A UART driver for QEMU RISC-V virt.
// Register offsets are byte-wide, NOT the ARM PL011 layout used elsewhere.
// LSR bit 5 (THRE) = transmitter holding register empty => ready to send.
// LSR bit 0 (DR)   = data ready => byte available to read.
inline static volatile uint8_t* reg(uint8_t off) {
    return (volatile uint8_t*)(BOARD_UART0_BASE + off);
}

void uart_init(void) {
    // Disable all interrupts
    *reg(1) = 0x00;
    // Enable DLAB to access divisor latches
    *reg(3) = 0x80;
    // Set baud rate divisor (10MHz / (16 * 115200) ≈ 5)
    uint16_t divisor = BOARD_SYSCLK_FREQ / (16 * BOARD_UART_BAUDRATE);
    *reg(0) = divisor & 0xFF;        // DLL (low byte)
    *reg(1) = (divisor >> 8) & 0xFF; // DLM (high byte)
    // 8N1 and clear DLAB
    *reg(3) = 0x03;
    // Enable FIFO, clear RX/TX FIFO, 14-byte trigger level
    *reg(2) = 0xC7;
    // RTS/DTR asserted (MCR)
    *reg(4) = 0x0B;
}

void uart_putc(char c) {
    // Wait until THR is empty (LSR bit 5)
    while ((*reg(5) & 0x20) == 0)
        ;
    *reg(0) = (uint8_t)c;
}

char uart_getc(void) {
    // Wait until data ready (LSR bit 0)
    while ((*reg(5) & 0x01) == 0)
        ;
    return (char)*reg(0);
}

void uart_puts(const char* s) {
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

int uart_getc_nb(char* c) {
    if (*reg(5) & 0x01) {
        *c = (char)*reg(0);
        return 1;
    }
    return 0;
}

#elif defined(SOC_CMSDK_APB_UART)

// ARM CMSDK APB UART 驱动 (QEMU mps2-an500 / MPS2+ AN500 FPGA image)。
// 寄存器布局: DATA +0x0, STATE +0x4, CTRL +0x8, INTSTATUS +0xC, BAUDDIV +0x10。
// STATE: bit0 TXFULL / bit1 RXFULL;  CTRL: bit0 TX_EN / bit1 RX_EN。
// 无 FIFO (单字符收发缓冲); BAUDDIV 必须 >= 16, 否则 QEMU 会静默丢弃 TX 输出。
inline static volatile uint32_t* ureg(uint32_t off) {
    return (volatile uint32_t*)(BOARD_UART0_BASE + off);
}

#define CMSDK_UART_DATA 0x00u
#define CMSDK_UART_STATE 0x04u
#define CMSDK_UART_CTRL 0x08u
#define CMSDK_UART_BAUDDIV 0x10u
#define CMSDK_STATE_TXFULL (1u << 0)
#define CMSDK_STATE_RXFULL (1u << 1)
#define CMSDK_CTRL_TX_EN (1u << 0)
#define CMSDK_CTRL_RX_EN (1u << 1)

void uart_init(void) {
    // 波特分频 = UART 时钟 (pclk, 25MHz) / 目标波特率
    *ureg(CMSDK_UART_BAUDDIV) = BOARD_SYSCLK_FREQ / BOARD_UART_BAUDRATE;
    *ureg(CMSDK_UART_CTRL) = CMSDK_CTRL_TX_EN | CMSDK_CTRL_RX_EN;
}

void uart_putc(char c) {
    while (*ureg(CMSDK_UART_STATE) & CMSDK_STATE_TXFULL)
        ;
    *ureg(CMSDK_UART_DATA) = (uint8_t)c;
}

char uart_getc(void) {
    while (!(*ureg(CMSDK_UART_STATE) & CMSDK_STATE_RXFULL))
        ;
    return (char)(*ureg(CMSDK_UART_DATA) & 0xFF);
}

void uart_puts(const char* s) {
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

int uart_getc_nb(char* c) {
    if (!(*ureg(CMSDK_UART_STATE) & CMSDK_STATE_RXFULL)) {
        return 0;
    }
    *c = (char)(*ureg(CMSDK_UART_DATA) & 0xFF);
    return 1;
}

#else

// 波特率与系统时钟统一取自 BSP (board.h)，更换板卡时无需改动驱动逻辑
void uart_init(void) {
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

void uart_putc(char c) {
    while (UART0_FR & UART_FR_TXFF)
        ;
    UART0_DR = c;
}

char uart_getc(void) {
    while (UART0_FR & UART_FR_RXFE)
        ;
    return (char)(UART0_DR & 0xFF);
}

void uart_puts(const char* s) {
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

// 非阻塞读取：如果有数据返回 1 并填入字符，否则立即返回 0
int uart_getc_nb(char* c) {
    if (UART0_FR & UART_FR_RXFE) {
        return 0; // 接收 FIFO 为空，直接返回
    }
    *c = (char)(UART0_DR & 0xFF);
    return 1;
}

#endif
