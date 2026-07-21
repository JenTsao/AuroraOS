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

        // 保留 1 个字节给末尾的 '\0'
        while (bytes_read < len - 1) {
            char c;
            if (uart_getc_nb(&c)) {
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
                // 没有输入时让出 CPU，通过 Syscall 休眠 5ms，避免忙等占用
                sys_sleep(5);
            }
        }
        return bytes_read;
    }
};

#endif
