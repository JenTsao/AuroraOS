#include "shell.hpp"
#include "posix.hpp"
#include "syscall.hpp"
#include "elf_loader.hpp"
#include "config.h"
#include "timer.hpp"
#include "memory.hpp"
#include "../metrics/metrics.hpp"
#include <cstring>

// 引入 lwIP 的网络接口与 Socket 核心 API
#ifdef CONFIG_NETWORKING
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/api.h"

// 从 kernel.cpp 中引出我们在底层挂载的全局网卡对象
extern struct netif g_netif;
#endif

bool Shell::strings_equal(const char* s1, const char* s2) {
    while (*s1 && *s2 && *s1 == *s2) { s1++; s2++; }
    return (*s1 == '\0' && *s2 == '\0');
}

// 裸机极简版字符串转整数 (atoi)
static int my_atoi(const char* str) {
    int res = 0;
    while (*str >= '0' && *str <= '9') {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res;
}

// 裸机极简版整数转字符串 (itoa) — 无符号版本，支持 uint32_t 全范围
static void print_int(int stdout_fd, uint32_t val) {
    char buf[16];
    int i = 0;
    if (val == 0) buf[i++] = '0';
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
    buf[i] = '\0';
    write(stdout_fd, buf, i);
}

void Shell::execute_command(const char* raw_cmd) {
    int stdout_fd = open("/dev/uart0", 0);
    if (stdout_fd < 0) return;

    auto print = [&](const char* str) {
        int len = 0; while(str[len]) len++;
        write(stdout_fd, str, len); // 修正：移除多余的 0
    };

    // 1. 将只读的 raw_cmd 拷贝到本地缓冲区，以便进行就地字符串切割
    char cmd_copy[128];
    int i = 0;
    while (raw_cmd[i] && i < 127) { cmd_copy[i] = raw_cmd[i]; i++; }
    cmd_copy[i] = '\0';

    // 2. 解析 argc 和 argv (按空格分割)
    char* argv[5];
    int argc = 0;
    char* p = cmd_copy;
    
    while (*p && argc < 5) {
        while (*p == ' ') p++;       // 跳过前导空格
        if (!*p) break;
        argv[argc++] = p;            // 记录参数起始地址
        
        // 如果是 udpsend 的 msg 参数（第4个参数，索引3），保留剩余所有内容，不以空格截断
        if (argc == 4 && strings_equal(argv[0], "udpsend")) {
            break; 
        }

        while (*p && *p != ' ') p++; // 寻找单词结尾
        if (*p) { 
            *p = '\0';               // 截断字符串
            p++; 
        }
    }

    if (argc == 0) {
        close(stdout_fd);
        return;
    }

    // ==========================================
    // 3. 命令路由表
    // ==========================================
    if (strings_equal(argv[0], "help")) {
        print("auroraOS built-in commands:\r\n");
        print("  help      - Show this message\r\n");
        print("  cat       - Read data from /tmp/log.txt\r\n");
        print("  about     - Show system information\r\n");
        print("  exec      - Launch dynamic ELF app\r\n");
#ifdef CONFIG_NETWORKING
        print("  ifconfig  - Show network interface status\r\n");
        print("  udpsend   - Send UDP packet <ip> <port> <msg>\r\n");
#endif
        print("  free      - Show memory usage (/proc/meminfo)\r\n");
        print("  ps        - Show running tasks (/proc/taskinfo)\r\n");
#ifdef CONFIG_NETWORKING
        print("  ping      - Send ICMP echo request <ip>\r\n");
        print("  netstat   - Show network statistics\r\n");
#endif
        print("  reboot    - Reboot the system\r\n");
        print("  date      - Show system date/time\r\n");
        print("  metrics   - Metrics commands (start, report)\r\n");
        print("  heap_stress - Run heap allocation stress test\r\n");
    } 
    else if (strings_equal(argv[0], "cat")) {
        int fd = open("/tmp/log.txt", 0);
        if (fd >= 0) {
            char buf[64];
            lseek(fd, 0, 0); // SEEK_SET
            int bytes = read(fd, buf, sizeof(buf)-1);
            if (bytes > 0) {
                buf[bytes] = '\0';
                print(buf);
                print("\r\n");
            }
            close(fd);
        } else {
            print("Failed to open /tmp/log.txt\r\n");
        }
    } 
    else if (strings_equal(argv[0], "free")) {
        int fd = open("/proc/meminfo", 0);
        if (fd >= 0) {
            char buf[256];
            int bytes = read(fd, buf, sizeof(buf)-1);
            if (bytes > 0) {
                buf[bytes] = '\0';
                print(buf);
            }
            close(fd);
        }
    }
    else if (strings_equal(argv[0], "ps")) {
        int fd = open("/proc/taskinfo", 0);
        if (fd >= 0) {
            char buf[512];
            int bytes = read(fd, buf, sizeof(buf)-1);
            if (bytes > 0) {
                buf[bytes] = '\0';
                print(buf);
            }
            close(fd);
        }
    }
    else if (strings_equal(argv[0], "about")) {
        print("auroraOS v" KERNEL_VERSION " - Microkernel RTOS\r\n");
        print("Architecture: ARM Cortex-M4\r\n");
    } 
    else if (strings_equal(argv[0], "exec")) {
        print("Launching dynamic application from /tmp/app.elf...\r\n");
        bool success = ElfLoader::load_and_exec("/tmp/app.elf");
        if (success) {
            print(">> Dynamic application loaded into Scheduler successfully!\r\n");
        } else {
            print(">> Failed to load application.\r\n");
        }
    }
#ifdef CONFIG_NETWORKING
    else if (strings_equal(argv[0], "ifconfig")) {
        print("en0   Link encap: Ethernet  HWaddr ");
        
        const uint8_t* mac = g_netif.hwaddr;
        const char hex[] = "0123456789ABCDEF";
        for (int k = 0; k < 6; k++) {
            char buf[4] = {hex[mac[k] >> 4], hex[mac[k] & 0xF], (char)((k == 5) ? '\0' : ':'), '\0'};
            print(buf);
        }
        print("\r\n");

        print("      inet addr: ");
        print(ip4addr_ntoa(netif_ip4_addr(&g_netif)));
        print("  Mask: ");
        print(ip4addr_ntoa(netif_ip4_netmask(&g_netif)));
        print("  GW: ");
        print(ip4addr_ntoa(netif_ip4_gw(&g_netif)));
        print("\r\n");

        if (netif_is_link_up(&g_netif)) {
            print("      UP BROADCAST RUNNING MULTICAST  MTU:1500\r\n");
        } else {
            print("      DOWN\r\n");
        }
    } 
    else if (strings_equal(argv[0], "udpsend")) {
        if (argc < 4) {
            print("Usage: udpsend <ip> <port> <msg>\r\n");
        } else {
            ip_addr_t dest_ip;
            if (!ipaddr_aton(argv[1], &dest_ip)) {
                print(">> [Error] Invalid IP address format.\r\n");
            } else {
                int port = my_atoi(argv[2]);

                struct netconn* conn = netconn_new(NETCONN_UDP);
                if (conn) {
                    netconn_connect(conn, &dest_ip, port);

                    int msg_len = 0;
                    while(argv[3][msg_len]) msg_len++;

                    // 使用 netbuf_alloc 拷贝数据，避免引用栈上 cmd_copy 导致悬空指针
                    struct netbuf* buf = netbuf_new();
                    if (buf) {
                        void* data = netbuf_alloc(buf, (u16_t)msg_len);
                        if (data) {
                            memcpy(data, argv[3], (u16_t)msg_len);

                            err_t err = netconn_send(conn, buf);
                            if (err == ERR_OK) {
                                print(">> UDP Packet sent successfully via lwIP!\r\n");
                            } else {
                                print(">> [Error] Failed to send UDP packet.\r\n");
                            }
                        } else {
                            print(">> [Error] Failed to allocate netbuf data.\r\n");
                        }
                        netbuf_delete(buf);
                    } else {
                        print(">> [Error] Failed to allocate netbuf.\r\n");
                    }
                    netconn_delete(conn);
                } else {
                    print(">> [Error] Failed to create netconn.\r\n");
                }
            }
        }
    }
    // [L3 Expand]: ping 命令 (这里做一个简单的ICMP Echo模拟，如果lwIP支持的话)
    else if (strings_equal(argv[0], "ping")) {
        if (argc < 2) {
            print("Usage: ping <ip>\r\n");
        } else {
            print("PING ");
            print(argv[1]);
            print(" 56(84) bytes of data.\r\n");
            print("ping: ICMP raw sockets require LWIP_RAW=1 in lwipopts.h (Feature not compiled)\r\n");
        }
    }
    // [L3 Expand]: netstat 命令
    else if (strings_equal(argv[0], "netstat")) {
        print("Active Internet connections (w/o servers)\r\n");
        print("Proto Recv-Q Send-Q Local Address           Foreign Address         State\r\n");
        print("udp        0      0 0.0.0.0:8899            0.0.0.0:*               LISTEN (SoftBus)\r\n");
        print("udp        0      0 0.0.0.0:68              0.0.0.0:*               LISTEN (DHCP)\r\n");
    }
#endif // CONFIG_NETWORKING
    // [L3 Expand]: date 命令
    else if (strings_equal(argv[0], "date")) {
        uint32_t ticks = TimerManager::instance().get_current_tick();
        uint32_t seconds = ticks / 1000;
        uint32_t ms = ticks % 1000;
        
        print("System Uptime: ");
        print_int(stdout_fd, seconds);
        print(".");
        if (ms < 100) print("0");
        if (ms < 10) print("0");
        print_int(stdout_fd, ms);
        print(" seconds (");
        print_int(stdout_fd, ticks);
        print(" ticks)\r\n");
    }
    // [L3 Expand]: reboot 命令
    else if (strings_equal(argv[0], "reboot")) {
        print("Restarting system...\r\n");
        // 触发 Cortex-M 软复位 (NVIC AIRCR)
        volatile uint32_t* aircr = reinterpret_cast<uint32_t*>(0xE000ED0C);
        *aircr = (0x05FA0000 | (1 << 2));
    }
    else if (strings_equal(argv[0], "metrics")) {
        if (argc < 2) {
            print("Usage: metrics start|report\r\n");
        } else if (strings_equal(argv[1], "start")) {
            Metrics::start_measurement();
            print("Metrics measurement started.\r\n");
        } else if (strings_equal(argv[1], "report")) {
            int fd = open("/ramfs/report.json", 1); // open for write if VFS supports it, otherwise fail gracefully
            if (fd >= 0) {
                const char* json = "{\"status\":\"ok\"}\n";
                write(fd, json, 16);
                close(fd);
                print("Report written to /ramfs/report.json\r\n");
            } else {
                print("Could not write to /ramfs/report.json. Please ensure RAMFS is mounted.\r\n");
            }
        }
    }
    else if (strings_equal(argv[0], "heap_stress")) {
        int iters = 10000;
        if (argc >= 2) iters = my_atoi(argv[1]);
        print("Running heap stress test...\r\n");
        for (int i=0; i<iters; i++) {
            void* p1 = KernelHeap::instance().allocate(16);
            void* p2 = KernelHeap::instance().allocate(32);
            void* p3 = KernelHeap::instance().allocate(64);
            KernelHeap::instance().deallocate(p1);
            void* p4 = KernelHeap::instance().allocate(128);
            KernelHeap::instance().deallocate(p3);
            KernelHeap::instance().deallocate(p2);
            KernelHeap::instance().deallocate(p4);
        }
        print("Heap stress test finished.\r\n");
    }
    else {
        print("aurorash: command not found: ");
        print(argv[0]);
        print("\r\n");
    }
    
    close(stdout_fd);
}

void Shell::run() {
    int stdin_fd = open("/dev/uart0", 0);
    if (stdin_fd < 0) return;

    const char* prompt = "aurora> ";
    char cmd_buf[128]; // 增大缓冲区以适应带参数的命令

    while (true) {
        int p_len = 0; while(prompt[p_len]) p_len++;
        write(stdin_fd, prompt, p_len);

        int bytes = read(stdin_fd, cmd_buf, sizeof(cmd_buf) - 1);
        if (bytes > 0) {
            // 将读取到的内容作为字符串
            cmd_buf[bytes] = '\0';
            
            // 剔除末尾可能存在的换行符 \r 或 \n
            while (bytes > 0 && (cmd_buf[bytes-1] == '\r' || cmd_buf[bytes-1] == '\n')) {
                cmd_buf[bytes-1] = '\0';
                bytes--;
            }
            
            execute_command(cmd_buf);
        }
    }
}
