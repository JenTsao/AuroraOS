#include "softbus.hpp"
#include "uart.h"
#include "vfs.hpp"

void SerialRpcBus::init() {
    service_count_ = 0;
}

// 极其精简的字符串比较（因为裸机环境没有 <string.h>）
bool SerialRpcBus::strings_equal(const char* s1, const char* s2) const {
    while (*s1 && *s2) {
        if (*s1 != *s2) return false;
        s1++; s2++;
    }
    return (*s1 == '\0' && *s2 == '\0');
}

// 验证凭证前缀并返回数据正文起始指针。
// 返回 nullptr 表示验证失败；返回非 nullptr 表示验证通过，
// 指向跳过 "AURORA_RPC_KEY" 及可选 ':' 分隔符后的数据正文。
// 这样 handler 收到的 payload 不含凭证信息，且凭证不会暴露给各服务。
const char* SerialRpcBus::verify_auth(const char* payload) const {
    const char* key = "AURORA_RPC_KEY";
    const char* p   = payload;
    while (*key) {
        if (*key != *p) return nullptr; // 前缀不匹配，拒绝
        key++;
        p++;
    }
    // 跳过可选的 ':' 或 ' ' 分隔符，使 handler 直接拿到业务数据
    if (*p == ':' || *p == ' ') p++;
    return p;
}

bool SerialRpcBus::register_service(const char* cmd, RpcCallback handler) {
    if (service_count_ >= MAX_RPC_HANDLERS) return false;
    services_[service_count_].cmd = cmd;
    services_[service_count_].handler = handler;
    service_count_++;
    return true;
}

void SerialRpcBus::send_request(const char* cmd, const char* payload) {
    LockGuard lock(tx_mutex_);
    int fd = VfsManager::instance().open("/dev/uart0", 0);
    if (fd >= 0) {
        char buf[256];
        int len = 0;
        auto append = [&](const char* s) { 
            while (*s && len < (int)sizeof(buf) - 3) buf[len++] = *s++; 
        };
        append("$RPC,");
        append(cmd);
        append(",");
        append(payload);
        buf[len++] = '#';
        buf[len++] = '\n';
        buf[len] = '\0';
        VfsManager::instance().write(fd, buf, len);
        VfsManager::instance().close(fd);
    }
}

void SerialRpcBus::dispatch(const char* cmd, const char* payload) {
    const char* data = verify_auth(payload);
    if (!data) {
        int fd = VfsManager::instance().open("/dev/uart0", 0);
        if (fd >= 0) {
            const char* msg = "[SerialRpcBus] Unauthorized RPC attempt blocked.\n";
            VfsManager::instance().write(fd, msg, 49);
            VfsManager::instance().close(fd);
        }
        return;
    }

    for (int i = 0; i < service_count_; i++) {
        if (strings_equal(services_[i].cmd, cmd)) {
            // 传入剥离凭证前缀后的数据正文，handler 无需自行解析前缀
            services_[i].handler(data);
            return;
        }
    }
    // 未知服务
    int fd = VfsManager::instance().open("/dev/uart0", 0);
    if (fd >= 0) {
        char buf[64];
        int len = 0;
        auto append = [&](const char* s) { 
            while (*s && len < (int)sizeof(buf) - 1) buf[len++] = *s++; 
        };
        append("[SerialRpcBus] Unknown Service requested: ");
        append(cmd);
        append("\n");
        VfsManager::instance().write(fd, buf, len);
        VfsManager::instance().close(fd);
    }
}

// 状态机解析帧格式: $RPC,CMD,PAYLOAD#
void SerialRpcBus::poll() {
    char c;
    if (!uart_getc_nb(&c)) return; // 总线无数据，立即让出 CPU

    switch (state_) {
        case 0: if (c == '$') state_ = 1; break;
        case 1: if (c == 'R') state_ = 2; else state_ = 0; break;
        case 2: if (c == 'P') state_ = 3; else state_ = 0; break;
        case 3: if (c == 'C') state_ = 4; else state_ = 0; break;
        case 4: 
            if (c == ',') { state_ = 5; cmd_idx_ = 0; } 
            else state_ = 0; 
            break;
        case 5: // 提取指令标识
            if (c == ',') {
                cmd_buf_[cmd_idx_] = '\0';
                state_ = 6;
                pay_idx_ = 0;
            } else if (cmd_idx_ < 15) {
                cmd_buf_[cmd_idx_++] = c;
            }
            break;
        case 6: // 提取数据负载，支持 COBS 转义或简单的 \x 逃逸，为简单起见，仅修复截断问题。
            // 真实项目中这里应处理转义字符，避免有效载荷中的 '#' 中断传输
            if (c == '#') {
                payload_buf_[pay_idx_] = '\0';
                dispatch(cmd_buf_, payload_buf_); // 派发执行
                state_ = 0; 
            } else if (pay_idx_ < 63) {
                payload_buf_[pay_idx_++] = c;
            } else {
                state_ = 7; // 溢出错误状态
            }
            break;
        case 7:
            if (c == '#') state_ = 0;
            break;
        default: state_ = 0; break;
    }
}
