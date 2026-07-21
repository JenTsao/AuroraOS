#ifndef AURORA_DISTRIBUTED_BUS_HPP
#define AURORA_DISTRIBUTED_BUS_HPP

#include <sys/time.h>
#include "lwip/sockets.h"
#include "task.hpp"
#include "posix.hpp"
#include <string.h>
#include <stdio.h> // For snprintf
#include "../utils/json_parser.hpp"
#include "../utils/hmac_sha256.hpp"   // HmacSha256, Crc32
#include "device_route_table.hpp"
#include "../metrics/metrics.hpp"
#include "../kernel/timer.hpp"


class DistributedSoftBus {
private:
    int udp_socket_;
    static constexpr uint16_t SOFTBUS_PORT = 8899;

    // ────────────────────────────────────────────────────────
    // 密鑰管理 — 支持双槽轮换
    // ────────────────────────────────────────────────────────
    struct KeySlot {
        uint8_t  key[32];   // HMAC-SHA256 pre-shared key
        uint32_t version;   // Monotonically increasing version
        bool     valid;
    };
    KeySlot key_slots_[2]{};
    uint8_t active_slot_ = 0;

    // Provision the active key slot.  Call once at init from a trusted source
    // (e.g., provisioned at manufacturing or via secure channel).
    void set_key(const uint8_t key[32], uint32_t version) noexcept {
        const uint8_t slot = active_slot_ ^ 1u;  // write to inactive slot
        memcpy(key_slots_[slot].key, key, 32u);
        key_slots_[slot].version = version;
        key_slots_[slot].valid   = true;
        active_slot_ = slot;  // atomic flip (single-core bare-metal)
    }

    // ────────────────────────────────────────────────────────
    // 防重放：滑动窗口序列号 (seq_num)
    // ────────────────────────────────────────────────────────
    static constexpr int REPLAY_WINDOW = 32;
    struct ReplayEntry {
        char     device_id[32];
        uint32_t last_seq;
        uint32_t last_active_tick;
        bool     valid;
    };
    ReplayEntry replay_table_[16]{};

    [[nodiscard]] bool check_and_update_seq(
            const char* device_id, uint32_t seq, uint32_t now_tick) noexcept
    {
        for (auto& e : replay_table_) {
            if (e.valid && strncmp(e.device_id, device_id, 31) == 0) {
                // Allow sequence reset or gap if device was inactive for > 30 seconds (30000 ticks)
                if (now_tick - e.last_active_tick > 30000u) {
                    e.last_seq = seq;
                    e.last_active_tick = now_tick;
                    return true;
                }
                if (seq <= e.last_seq)         return false;  // replay!
                if (seq > e.last_seq + static_cast<uint32_t>(REPLAY_WINDOW))
                                               return false;  // gap too large
                e.last_seq = seq;
                e.last_active_tick = now_tick;
                return true;
            }
        }
        // New device: find empty slot
        for (auto& e : replay_table_) {
            if (!e.valid) {
                strncpy(e.device_id, device_id, 31);
                e.device_id[31] = '\0';
                e.last_seq = seq;
                e.last_active_tick = now_tick;
                e.valid    = true;
                return true;
            }
        }
        return false;  // table full, reject
    }

    // ────────────────────────────────────────────────────────
    // 速率限制：令牌桶 (Token Bucket) 每源 IP
    // ────────────────────────────────────────────────────────
    struct RateBucket {
        char     ip[16];
        uint32_t tokens;              // Current token count
        uint32_t last_refill_tick;    // Tick of last refill
        bool     valid;
        static constexpr uint32_t CAPACITY    = 10u;  // Bucket capacity
        static constexpr uint32_t RATE_PER_S  =  1u;  // Tokens per second
    };
    RateBucket rate_buckets_[8]{};

    [[nodiscard]] bool rate_limit_pass(
            const char* ip, uint32_t now_tick) noexcept
    {
        for (auto& b : rate_buckets_) {
            if (b.valid && strncmp(b.ip, ip, 15) == 0) {
                // Refill: 1 token per 1000 ticks (1 Hz at 1 kHz tick rate)
                const uint32_t elapsed = now_tick - b.last_refill_tick;
                const uint32_t added   = elapsed / 1000u * RateBucket::RATE_PER_S;
                if (added > 0u) {
                    b.tokens = (b.tokens + added < RateBucket::CAPACITY)
                             ? b.tokens + added : RateBucket::CAPACITY;
                    b.last_refill_tick = now_tick;
                }
                if (b.tokens == 0u) return false;  // rate-limited
                b.tokens--;
                return true;
            }
        }
        // New IP: register
        for (auto& b : rate_buckets_) {
            if (!b.valid) {
                strncpy(b.ip, ip, 15); b.ip[15] = '\0';
                b.tokens           = RateBucket::CAPACITY - 1u;
                b.last_refill_tick = now_tick;
                b.valid            = true;
                return true;
            }
        }
        return false;  // table full
    }

    // ────────────────────────────────────────────────────────
    // HMAC-SHA256 验证 + 防重放序列号混入
    //
    // hmac_hex : 64-char hex-encoded HMAC-SHA256 digest
    // challenge: device_id string
    // seq      : uint32 sequence number (big-endian 4 bytes mixed into HMAC msg)
    // ────────────────────────────────────────────────────────
    [[nodiscard]] bool verify_hmac(
            const char* challenge, const char* hmac_hex,
            uint32_t seq) noexcept
    {
        if (!challenge || !hmac_hex) return false;
        const KeySlot& slot = key_slots_[active_slot_];
        if (!slot.valid) return false;

        // Build HMAC message: challenge bytes || seq (4 bytes big-endian)
        const size_t chal_len = strnlen(challenge, 31u);
        const uint8_t seq_bytes[4] = {
            static_cast<uint8_t>(seq >> 24),
            static_cast<uint8_t>(seq >> 16),
            static_cast<uint8_t>(seq >>  8),
            static_cast<uint8_t>(seq)
        };

        HmacSha256 ctx;
        ctx.init(slot.key, 32u);
        ctx.update(reinterpret_cast<const uint8_t*>(challenge), chal_len);
        ctx.update(seq_bytes, 4u);
        uint8_t computed[32]{};
        ctx.finish(computed);

        // Decode the 64-char hex string into 32 bytes
        uint8_t received[32]{};
        for (int i = 0; i < 32; i++) {
            auto hex_nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex_nibble(hmac_hex[i*2]);
            const int lo = hex_nibble(hmac_hex[i*2+1]);
            if (hi < 0 || lo < 0) return false;  // invalid hex
            received[i] = static_cast<uint8_t>((hi << 4) | lo);
        }

        // Constant-time comparison (prevent timing side-channel)
        return HmacSha256::constant_time_eq(computed, received, 32u);
    }

    bool validate_cap(const char* cap) noexcept {
        int len = static_cast<int>(strnlen(cap, 64));
        if (len == 0 || len == 64) return false;
        for (int i = 0; i < len; i++) {
            char c = cap[i];
            if (!((c >= 'a' && c <= 'z') || c == '_' || c == ',' ||
                   c == '[' || c == ']' || c == '"')) {
                return false;
            }
        }
        return true;
    }

    bool validate_device_id(const char* id) noexcept {
        int len = static_cast<int>(strnlen(id, 32));
        if (len == 0 || len == 32) return false;
        for (int i = 0; i < len; i++) {
            char c = id[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                return false;
            }
        }
        return true;
    }


public:
    static DistributedSoftBus& instance() {
        static DistributedSoftBus bus;
        return bus;
    }

    void init() {
#ifdef DEBUG_BYPASS_SOFTBUS_KEY
#warning "Using hardcoded SoftBus key for debugging! Do not use in production."
        const uint8_t default_key[32] = {
            0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
            0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
            0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
            0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38
        };
        set_key(default_key, 1);
#else
        // TODO: Read actual device key from Secure Element or encrypted OTP partition
        // uint8_t key[32]; SecureStorage::read_softbus_key(key); set_key(key, version);
#endif

        // 1. 创建 UDP Socket
        udp_socket_ = lwip_socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_socket_ < 0) return;

        // 2. 开启 Socket 的广播权限
        int broadcast_enable = 1;
        lwip_setsockopt(udp_socket_, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

        // 3. 禁用自发广播/组播的本地环回 (防止自己收到自己的信标)
        char loopch = 0;
        lwip_setsockopt(udp_socket_, IPPROTO_IP, IP_MULTICAST_LOOP, &loopch, sizeof(loopch));

        // 4. 绑定本地端口，准备接收其他设备的广播
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = lwip_htons(SOFTBUS_PORT);
        local_addr.sin_addr.s_addr = lwip_htonl(INADDR_ANY);

        lwip_bind(udp_socket_, (struct sockaddr*)&local_addr, sizeof(local_addr));
    }

    // ========================================================
    // 发送广播信标 (由软件定时器每 3 秒调用一次)
    // ========================================================
    void broadcast_beacon() {
        if (udp_socket_ < 0) return;

        struct sockaddr_in broadcast_addr;
        memset(&broadcast_addr, 0, sizeof(broadcast_addr));
        broadcast_addr.sin_family      = AF_INET;
        broadcast_addr.sin_port        = lwip_htons(SOFTBUS_PORT);
        broadcast_addr.sin_addr.s_addr = lwip_htonl(INADDR_BROADCAST);

        static uint32_t beacon_seq = 1;
        uint32_t seq = beacon_seq++;
        
        const KeySlot& slot = key_slots_[active_slot_];
        const char* challenge = "aurorawatch";
        const uint8_t seq_bytes[4] = {
            static_cast<uint8_t>(seq >> 24), static_cast<uint8_t>(seq >> 16),
            static_cast<uint8_t>(seq >>  8), static_cast<uint8_t>(seq)
        };
        
        HmacSha256 ctx;
        ctx.init(slot.key, 32u);
        ctx.update(reinterpret_cast<const uint8_t*>(challenge), 11);
        ctx.update(seq_bytes, 4u);
        uint8_t computed[32]{};
        ctx.finish(computed);
        
        char auth_hex[65] = {0};
        const char* hex_chars = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            auth_hex[i*2] = hex_chars[(computed[i] >> 4) & 0x0F];
            auth_hex[i*2+1] = hex_chars[computed[i] & 0x0F];
        }
        
        char beacon_payload[256];
        snprintf(beacon_payload, sizeof(beacon_payload),
            "{\"event\":\"beacon\",\"device_id\":\"%s\","
            "\"cap\":[\"display\",\"touch\"],\"seq\":\"%u\",\"auth\":\"%s\"}",
            challenge, seq, auth_hex);

        lwip_sendto(udp_socket_, beacon_payload,
                    strlen(beacon_payload), 0,
                    (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
    }


    // ========================================================
    // 软总线监听线程核心逻辑 (阻塞接收网络报文)
    // ========================================================
    void listener_task() {
        char recv_buf[256];
        struct sockaddr_in remote_addr;
        socklen_t addr_len = sizeof(remote_addr);

        int console_fd = open("/dev/uart0", 0);
        write(console_fd, "[SoftBus] Distributed Engine Listening on UDP 8899...\r\n", 55);
        close(console_fd);

        while (true) {
            int bytes = lwip_recvfrom(udp_socket_, recv_buf, sizeof(recv_buf) - 1, 0,
                                      (struct sockaddr*)&remote_addr, &addr_len);

            if (bytes > 0) {
                recv_buf[bytes] = '\0';

                char ip_str[16];
                ip4addr_ntoa_r((const ip4_addr_t*)&remote_addr.sin_addr,
                               ip_str, sizeof(ip_str));

                // ─ 速率限制：每源 IP 令牌桶检查 ───────────────
                const uint32_t now_tick =
                    TimerManager::instance().get_current_tick();
                if (!rate_limit_pass(ip_str, now_tick)) {
                    // Silently drop — DoS mitigation
                    Metrics::inc_net_drop();
                    continue;
                }

                // ─ JSON 解析 ─────────────────────────────────
                JsonParser parser(recv_buf);
                char event_type[32]  = {0};
                char device_id[32]   = {0};
                char cap_array[64]   = {0};
                char auth_token[65]  = {0};  // 64-char hex HMAC + NUL
                char seq_str[12]     = {0};  // uint32 as decimal string

                if (parser.get_string("event",     event_type, 32) &&
                    strcmp(event_type, "beacon") == 0 &&
                    parser.get_string("device_id", device_id,  32) &&
                    validate_device_id(device_id) &&
                    parser.get_raw_value("cap",    cap_array,  64) &&
                    validate_cap(cap_array) &&
                    parser.get_string("auth",      auth_token, 65) &&
                    parser.get_string("seq",       seq_str,    12))
                {
                    // Parse seq_num
                    uint32_t seq = 0u;
                    for (int i = 0; seq_str[i] >= '0' && seq_str[i] <= '9'; i++)
                        seq = seq * 10u + static_cast<uint32_t>(seq_str[i] - '0');

                    // 验签 + 防重放
                    if (verify_hmac(device_id, auth_token, seq) &&
                        check_and_update_seq(device_id, seq, now_tick))
                    {
                        DeviceRouteTable::instance().register_or_update_device(
                            ip_str, device_id, cap_array, now_tick);
                        Metrics::inc_softbus_register();
                    }
                }
            }
        }
    }
};


#endif
