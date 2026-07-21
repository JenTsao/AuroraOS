#ifndef AURORA_UART_DEVICE_HPP
#define AURORA_UART_DEVICE_HPP

#include "device.hpp"
#include "uart.h"     // 引入底层的 C 驱动接口
#include "mutex.hpp"  // 引入互斥锁防止并发打印乱码
#include "syscall.hpp" // 引入 sys_sleep 等系统调用

class UartDevice : public CharDevice {
private:
    Mutex tx_mutex_; // 串口发送互斥锁

public:
    // 构造函数，将设备名传给基类
    UartDevice(const char* name) : CharDevice(name) {}

    // 设备打开时的回调
    int open() override {
        // uart_init() 已在 kernel_main 全局初始化过
        return 0; 
    }

    int close() override {
        return 0;
    }

    // 实现 POSIX 风格的 write
    int write(const char* buf, int len, int offset, void* priv) override {
        (void)offset; // UART is a stream device, ignore offset
        LockGuard lock(tx_mutex_);
        return write_internal(buf, len);
    }

    int write_internal(const char* buf, int len) {
        for (int i = 0; i < len; i++) {
            // 自动补充 \r，适配部分终端
            if (buf[i] == '\n') {
                uart_putc('\r');
            }
            uart_putc(buf[i]);
        }
        return len;
    }

    // 实现 POSIX 风格的 read，并自带行缓冲和退格回显功能
    int read(char* buf, int len, int offset, void* priv) override {
        (void)offset;
        int bytes_read = 0;

        // ── DIAGNOSTIC PROBE (临时) ── read() 进入点。RDL2 标记表示已含 -display none
        //    修复的新 ELF；若仍见 RDL(无2) 说明 CI 用了缓存旧 ELF，需清缓存重编。
        uart_puts("RDL2\r\n");

        // ── DIAGNOSTIC PROBE (临时) ── 进入即读 UART0_FR，不依赖循环/睡眠节奏。
        //    RFR=0xFFFFFFFF => UART 未上钟 (RCGCUART 未开，读未门控外设返回全1)；
        //    RFR 正常但 RXFE=1 => FIFO 空，QEMU 没把 pexpect 数据送进 RX；
        //    RXFE=0 => 数据已在 FIFO，根因在别处。
        {
            uint32_t fr0 = UART0_FR;
            uart_puts("RFR=0x");
            for (int s = 28; s >= 0; s -= 4) {
                uint32_t nib = (fr0 >> s) & 0xF;
                uart_putc(nib < 10 ? (char)('0' + nib) : (char)('A' + nib - 10));
            }
            uart_puts(" RXFE=");
            uart_putc((fr0 & UART_FR_RXFE) ? '1' : '0');
            uart_puts("\r\n");
        }

        static uint32_t rx_poll = 0; // 轮询计数，仅用于节流打印 FR

        // 保留 1 个字节给末尾的 '\0'
        while (bytes_read < len - 1) {
            char c;
            if (uart_getc_nb(&c)) {
                // ── DIAGNOSTIC PROBE (临时) ── 收到字符，证明 RX 通路打通
                uart_puts("RX:");
                uart_putc(c);
                uart_puts("\r\n");
                if (c == '\r' || c == '\n') {
                    buf[bytes_read] = '\0';
                    write_internal("\n", 1); // 回显换行
                    break;
                } else if (c == '\b' || c == 127) { // 处理退格键 (Backspace)
                    if (bytes_read > 0) {
                        bytes_read--;
                        // 在终端上抹掉这个字符
                        write_internal("\b \b", 3);
                    }
                } else {
                    buf[bytes_read++] = c;
                    write_internal(&c, 1); // 正常字符立刻回显到屏幕
                }
            } else {
                // ── DIAGNOSTIC PROBE (临时) ── 前 5 次空轮询即打印 UART0_FR。
                //    去掉 100 次节流，避免 sys_sleep 节拍过粗导致阈值到不了。
                if (rx_poll < 5) {
                    uint32_t fr = UART0_FR;
                    uart_puts("FR=0x");
                    for (int s = 28; s >= 0; s -= 4) {
                        uint32_t nib = (fr >> s) & 0xF;
                        uart_putc(nib < 10 ? (char)('0' + nib) : (char)('A' + nib - 10));
                    }
                    uart_puts(" RXFE=");
                    uart_putc((fr & UART_FR_RXFE) ? '1' : '0');
                    uart_puts("\r\n");
                }
                rx_poll++;
                // 如果当前没有敲击键盘，立刻让出 CPU，通过 Syscall 休眠 5ms
                sys_sleep(5);
            }
        }
        return bytes_read;
    }
};

#endif
