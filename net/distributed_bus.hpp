#ifndef AURORA_DISTRIBUTED_BUS_HPP
#define AURORA_DISTRIBUTED_BUS_HPP

#include <sys/time.h>
#include "net_client.hpp"
#include "protocol_registry.hpp"
#include "task.hpp"
#include "posix.hpp"
#include <string.h>
#include <stdio.h> // For snprintf
#include "../utils/json_parser.hpp"
#include "../utils/hmac_sha256.hpp" // HmacSha256, Crc32
#include "device_route_table.hpp"
#include "../metrics/metrics.hpp"
#include "../kernel/interrupt/timer.hpp"
#include "../kernel/core/arch_api.hpp"
#include "../hal/secure_storage_hal.hpp"

class DistributedSoftBus : public auroraos::net::IProtocolHandler {
public:
    static constexpr uint16_t SOFTBUS_PORT = 8899;

    // 供上层服务（如 February AI 引擎）挂载的二进制意图帧与节点发现回调。
    // 保持函数指针接口，避免 net/ 反向依赖 ai/。
    using IntentFrameHandler = void (*)(int session_id, const uint8_t* data, size_t len, void* user);
    using PeerDiscoveryHandler = void (*)(const char* device_id, const char* ip_str, uint32_t now_tick, void* user);
    using TxFrameHook = int (*)(const void* data, size_t len, void* user);

private:
    int udp_socket_ = -1;
    char local_device_id_[32]{};
    IntentFrameHandler intent_frame_handler_ = nullptr;
    void* intent_frame_user_ = nullptr;
    PeerDiscoveryHandler peer_discovery_handler_ = nullptr;
    void* peer_discovery_user_ = nullptr;
    TxFrameHook tx_hook_ = nullptr;
    void* tx_hook_user_ = nullptr;

    // ────────────────────────────────────────────────────────
    // 密钥管理 — 支持双槽轮换
    // ────────────────────────────────────────────────────────
    struct KeySlot {
        uint8_t key[32];  // HMAC-SHA256 pre-shared key
        uint32_t version; // Monotonically increasing version
        bool valid;
    };

    KeySlot key_slots_[2]{};
    uint8_t active_slot_ = 0;

    // Provision the active key slot.  Call once at init from a trusted source
    // (e.g., provisioned at manufacturing or via secure channel).
    void set_key(const uint8_t key[32], uint32_t version) noexcept {
        const uint8_t slot = active_slot_ ^ 1u; // write to inactive slot
        memcpy(key_slots_[slot].key, key, 32u);
        key_slots_[slot].version = version;
        key_slots_[slot].valid = true;
        active_slot_ = slot; // atomic flip (single-core bare-metal)
    }

    // ────────────────────────────────────────────────────────
    // 防重放：滑动窗口序列号(seq_num)
    // ────────────────────────────────────────────────────────
    static constexpr int REPLAY_WINDOW = 32;

    struct ReplayEntry {
        char device_id[32];
        uint32_t last_seq;
        uint32_t last_active_tick;
        bool valid;
    };

    ReplayEntry replay_table_[16]{};

    [[nodiscard]] bool check_and_update_seq(const char* device_id, uint32_t seq, uint32_t now_tick) noexcept {
        for (auto& e : replay_table_) {
            if (e.valid && strncmp(e.device_id, device_id, 31) == 0) {
                // Allow sequence reset or gap if device was inactive for > 30 seconds (30000 ticks)
                if (now_tick - e.last_active_tick > 30000u) {
                    e.last_seq = seq;
                    e.last_active_tick = now_tick;
                    return true;
                }
                if (seq <= e.last_seq)
                    return false; // replay!
                if (seq > e.last_seq + static_cast<uint32_t>(REPLAY_WINDOW))
                    return false; // gap too large
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
                e.valid = true;
                return true;
            }
        }
        return false; // table full, reject
    }

    // ────────────────────────────────────────────────────────
    // 速率限制：令牌桶 (Token Bucket) 每源 IP
    // ────────────────────────────────────────────────────────
    struct RateBucket {
        char ip[16];
        uint32_t tokens;           // Current token count
        uint32_t last_refill_tick; // Tick of last refill
        bool valid;
        static constexpr uint32_t CAPACITY = 10u;  // Bucket capacity
        static constexpr uint32_t RATE_PER_S = 1u; // Tokens per second
    };

    RateBucket rate_buckets_[8]{};

    [[nodiscard]] bool rate_limit_pass(const char* ip, uint32_t now_tick) noexcept {
        for (auto& b : rate_buckets_) {
            if (b.valid && strncmp(b.ip, ip, 15) == 0) {
                // Refill: 1 token per 1000 ticks (1 Hz at 1 kHz tick rate)
                const uint32_t elapsed = now_tick - b.last_refill_tick;
                const uint32_t added = elapsed / 1000u * RateBucket::RATE_PER_S;
                if (added > 0u) {
                    b.tokens = (b.tokens + added < RateBucket::CAPACITY) ? b.tokens + added : RateBucket::CAPACITY;
                    b.last_refill_tick = now_tick;
                }
                if (b.tokens == 0u)
                    return false; // rate-limited
                b.tokens--;
                return true;
            }
        }
        // New IP: register
        for (auto& b : rate_buckets_) {
            if (!b.valid) {
                strncpy(b.ip, ip, 15);
                b.ip[15] = '\0';
                b.tokens = RateBucket::CAPACITY - 1u;
                b.last_refill_tick = now_tick;
                b.valid = true;
                return true;
            }
        }
        return false; // table full
    }

    // ────────────────────────────────────────────────────────
    // HMAC-SHA256 验证 + 防重放序列号混入
    //
    // hmac_hex : 64-char hex-encoded HMAC-SHA256 digest
    // challenge: device_id string
    // seq      : uint32 sequence number (big-endian 4 bytes mixed into HMAC msg)
    // ────────────────────────────────────────────────────────
    [[nodiscard]] bool verify_hmac(const char* challenge, const char* hmac_hex, uint32_t seq) noexcept {
        if (!challenge || !hmac_hex)
            return false;
        const KeySlot& slot = key_slots_[active_slot_];
        if (!slot.valid)
            return false;

        // Build HMAC message: challenge bytes || seq (4 bytes big-endian)
        const size_t chal_len = strnlen(challenge, 31u);
        const uint8_t seq_bytes[4] = {static_cast<uint8_t>(seq >> 24), static_cast<uint8_t>(seq >> 16),
                                      static_cast<uint8_t>(seq >> 8), static_cast<uint8_t>(seq)};

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
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            const int hi = hex_nibble(hmac_hex[i * 2]);
            const int lo = hex_nibble(hmac_hex[i * 2 + 1]);
            if (hi < 0 || lo < 0)
                return false; // invalid hex
            received[i] = static_cast<uint8_t>((hi << 4) | lo);
        }

        // Constant-time comparison (prevent timing side-channel)
        return HmacSha256::constant_time_eq(computed, received, 32u);
    }

    bool validate_cap(const char* cap) noexcept {
        int len = static_cast<int>(strnlen(cap, 64));
        if (len == 0 || len == 64)
            return false;
        for (int i = 0; i < len; i++) {
            char c = cap[i];
            if (!((c >= 'a' && c <= 'z') || c == '_' || c == ',' || c == '[' || c == ']' || c == '"')) {
                return false;
            }
        }
        return true;
    }

    bool validate_device_id(const char* id) noexcept {
        int len = static_cast<int>(strnlen(id, 32));
        if (len == 0 || len == 32)
            return false;
        for (int i = 0; i < len; i++) {
            char c = id[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
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

    void provision_key(const uint8_t key[32], uint32_t version) noexcept {
        if (key) {
            set_key(key, version);
        }
    }

    void reset_security_state() noexcept {
        memset(replay_table_, 0, sizeof(replay_table_));
        memset(rate_buckets_, 0, sizeof(rate_buckets_));
    }

    void set_intent_frame_handler(IntentFrameHandler fn, void* user = nullptr) noexcept {
        intent_frame_handler_ = fn;
        intent_frame_user_ = user;
    }

    void set_peer_discovery_handler(PeerDiscoveryHandler fn, void* user = nullptr) noexcept {
        peer_discovery_handler_ = fn;
        peer_discovery_user_ = user;
    }

    void set_tx_hook(TxFrameHook fn, void* user = nullptr) noexcept {
        tx_hook_ = fn;
        tx_hook_user_ = user;
    }

    bool process_intent_frame(const char* ip_str, const uint8_t* payload, size_t payload_len,
                              uint32_t now_tick) noexcept {
        // February SoftBus binary frame magic: 0xFB02 (little-endian: 0x02, 0xFB)
        if (!ip_str || !payload || payload_len < 4 || payload[0] != 0x02u || payload[1] != 0xFBu) {
            return false;
        }
        if (!rate_limit_pass(ip_str, now_tick)) {
            Metrics::inc_net_drop();
            return false;
        }
        if (intent_frame_handler_) {
            intent_frame_handler_(1, payload, payload_len, intent_frame_user_);
        }
        return true;
    }

    int send_raw_frame(const void* data, size_t len) noexcept {
        if (!data || len == 0) {
            return -1;
        }
        if (tx_hook_) {
            return tx_hook_(data, len, tx_hook_user_);
        }
        if (udp_socket_ < 0) {
            return -1;
        }
        struct sockaddr_in broadcast_addr{};
        broadcast_addr.sin_family = AF_INET;
        broadcast_addr.sin_port = htons(SOFTBUS_PORT);
        broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        return auroraos::net::net_sendto(udp_socket_, data, len, 0,
                                         reinterpret_cast<struct sockaddr*>(&broadcast_addr), sizeof(broadcast_addr));
    }

    static void format_u8_dec(char*& p, char* end, uint8_t v) noexcept {
        if (v >= 100u && p < end) {
            *p++ = static_cast<char>('0' + (v / 100u));
            v %= 100u;
            if (p < end) *p++ = static_cast<char>('0' + (v / 10u));
            if (p < end) *p++ = static_cast<char>('0' + (v % 10u));
        } else if (v >= 10u && p < end) {
            *p++ = static_cast<char>('0' + (v / 10u));
            if (p < end) *p++ = static_cast<char>('0' + (v % 10u));
        } else if (p < end) {
            *p++ = static_cast<char>('0' + v);
        }
    }

    static void format_ipv4(char out[16], uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) noexcept {
        char* p = out;
        char* end = out + 15;
        format_u8_dec(p, end, b0);
        if (p < end) *p++ = '.';
        format_u8_dec(p, end, b1);
        if (p < end) *p++ = '.';
        format_u8_dec(p, end, b2);
        if (p < end) *p++ = '.';
        format_u8_dec(p, end, b3);
        *p = '\0';
    }

    // ProtocolRegistry IProtocolHandler 接口：作为可插拔 L4 协议监听 UDP 8899
    const char* name() const noexcept override {
        return "softbus_udp";
    }

    auroraos::net::ProtocolVerdict on_packet(const auroraos::net::L4PacketContext& ctx, const uint8_t* payload,
                                             size_t payload_len) noexcept override {
        if (!payload || payload_len == 0 || ctx.dst_port != SOFTBUS_PORT) {
            return auroraos::net::ProtocolVerdict::Pass;
        }

        // ctx.src_ip 为主机字节序 (b0 << 24 | b1 << 16 | b2 << 8 | b3)
        char ip_str[16]{};
        format_ipv4(ip_str, static_cast<uint8_t>((ctx.src_ip >> 24) & 0xFFu),
                    static_cast<uint8_t>((ctx.src_ip >> 16) & 0xFFu),
                    static_cast<uint8_t>((ctx.src_ip >> 8) & 0xFFu),
                    static_cast<uint8_t>(ctx.src_ip & 0xFFu));

        const uint32_t now_tick = TimerManager::instance().get_current_tick();
        if (payload_len >= 4 && payload[0] == 0x02u && payload[1] == 0xFBu) {
            (void)process_intent_frame(ip_str, payload, payload_len, now_tick);
            return auroraos::net::ProtocolVerdict::Pass;
        }

        char recv_buf[256]{};
        const size_t copy_len = (payload_len < sizeof(recv_buf) - 1) ? payload_len : (sizeof(recv_buf) - 1);
        memcpy(recv_buf, payload, copy_len);
        recv_buf[copy_len] = '\0';

        (void)process_beacon_message(ip_str, recv_buf, now_tick);
        return auroraos::net::ProtocolVerdict::Pass;
    }

    bool process_beacon_message(const char* ip_str, const char* recv_buf, uint32_t now_tick) noexcept {
        if (!ip_str || !recv_buf) {
            return false;
        }
        if (!rate_limit_pass(ip_str, now_tick)) {
            Metrics::inc_net_drop();
            return false;
        }

        JsonParser parser(recv_buf);
        char event_type[32] = {0};
        char device_id[32] = {0};
        char cap_array[64] = {0};
        char auth_token[65] = {0}; // 64-char hex HMAC + NUL
        char seq_str[12] = {0};    // uint32 as decimal string

        if (parser.get_string("event", event_type, 32) && strcmp(event_type, "beacon") == 0 &&
            parser.get_string("device_id", device_id, 32) && validate_device_id(device_id) &&
            parser.get_raw_value("cap", cap_array, 64) && validate_cap(cap_array) &&
            parser.get_string("auth", auth_token, 65) && parser.get_string("seq", seq_str, 12)) {
            uint32_t seq = 0u;
            for (int i = 0; seq_str[i] >= '0' && seq_str[i] <= '9'; i++) {
                seq = seq * 10u + static_cast<uint32_t>(seq_str[i] - '0');
            }

            if (verify_hmac(device_id, auth_token, seq) && check_and_update_seq(device_id, seq, now_tick)) {
                DeviceRouteTable::instance().register_or_update_device(ip_str, device_id, cap_array, now_tick);
                Metrics::inc_softbus_register();
                if (peer_discovery_handler_) {
                    peer_discovery_handler_(device_id, ip_str, now_tick, peer_discovery_user_);
                }
                return true;
            }
        }
        return false;
    }

    void init() {
        // 注册为可插拔 L4 UDP 协议处理器（端口 8899）
        auroraos::net::ProtocolRegistry::instance().register_handler(auroraos::net::PROTO_UDP, SOFTBUS_PORT, this,
                                                                     auroraos::net::ProtocolRegistry::PRIORITY_SERVICE);

#ifdef DEBUG_BYPASS_SOFTBUS_KEY
#warning "Using hardcoded SoftBus key for debugging! Do not use in production."
        const uint8_t default_key[32] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x11, 0x12, 0x13,
                                         0x14, 0x15, 0x16, 0x17, 0x18, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
                                         0x27, 0x28, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
        set_key(default_key, 1);
#else
        // Production path: load the per-device key from Secure Element /
        // encrypted OTP via the Secure Storage HAL. Fail closed: if the HAL
        // is absent (weak default returns nullptr) or reports "not
        // provisioned", key_slots_ stays {valid=false} so verify_hmac()
        // rejects every peer and broadcast_beacon() stays silent.
        uint8_t provisioned_key[32];
        uint32_t provisioned_version = 0;
        auroraos::hal::ISecureStorageHal* secure = auroraos::hal::get_secure_storage_hal();
        if (secure != nullptr && secure->read_softbus_key(provisioned_key, &provisioned_version)) {
            set_key(provisioned_key, provisioned_version);
        }
#endif

        // Generate a dynamic device ID for this session using CPU cycle counter for entropy
        uint32_t uid = Arch::get_cycle();
        snprintf(local_device_id_, sizeof(local_device_id_), "device_%08lx", (unsigned long)uid);

        // 1. 关闭 Socket（如果存在），防止文件描述符泄漏
        if (udp_socket_ >= 0) {
            auroraos::net::net_close(udp_socket_);
            udp_socket_ = -1;
        }

        // 2. 创建 UDP Socket
        udp_socket_ = auroraos::net::net_socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_socket_ < 0)
            return;

        // 3. 开启 Socket 的广播权限
        int broadcast_enable = 1;
        auroraos::net::net_setsockopt(udp_socket_, SOL_SOCKET, SO_BROADCAST, &broadcast_enable,
                                      sizeof(broadcast_enable));

        // 4. 禁用自发广播/组播的本地环路(防止自己收到自己的消息)
#ifdef IP_MULTICAST_LOOP
        char loopch = 0;
        auroraos::net::net_setsockopt(udp_socket_, IPPROTO_IP, IP_MULTICAST_LOOP, &loopch, sizeof(loopch));
#endif

        // 5. 绑定本地端口，准备接收其他设备的广播
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(SOFTBUS_PORT);
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        auroraos::net::net_bind(udp_socket_, (struct sockaddr*)&local_addr, sizeof(local_addr));
    }

    // ========================================================
    // 发送广播消息(由软件定时器每 3 秒调用一次)
    // ========================================================
    void broadcast_beacon() {
        if (udp_socket_ < 0)
            return;

        const KeySlot& slot = key_slots_[active_slot_];
        if (!slot.valid) {
            // Never broadcast an auth field computed from an unprovisioned
            // (all-zero) key — mirrors the fail-closed check in verify_hmac().
            return;
        }

        struct sockaddr_in broadcast_addr;
        memset(&broadcast_addr, 0, sizeof(broadcast_addr));
        broadcast_addr.sin_family = AF_INET;
        broadcast_addr.sin_port = htons(SOFTBUS_PORT);
        broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        static uint32_t beacon_seq = 1;
        uint32_t seq = beacon_seq++;

        const char* challenge = local_device_id_;
        const uint8_t seq_bytes[4] = {static_cast<uint8_t>(seq >> 24), static_cast<uint8_t>(seq >> 16),
                                      static_cast<uint8_t>(seq >> 8), static_cast<uint8_t>(seq)};

        HmacSha256 ctx;
        ctx.init(slot.key, 32u);
        ctx.update(reinterpret_cast<const uint8_t*>(challenge), strnlen(challenge, 31u));
        ctx.update(seq_bytes, 4u);
        uint8_t computed[32]{};
        ctx.finish(computed);

        char auth_hex[65] = {0};
        const char* hex_chars = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            auth_hex[i * 2] = hex_chars[(computed[i] >> 4) & 0x0F];
            auth_hex[i * 2 + 1] = hex_chars[computed[i] & 0x0F];
        }

        char beacon_payload[256];
        snprintf(beacon_payload, sizeof(beacon_payload),
                 "{\"event\":\"beacon\",\"device_id\":\"%s\","
                 "\"cap\":[\"display\",\"touch\"],\"seq\":\"%lu\",\"auth\":\"%s\"}",
                 challenge, (unsigned long)seq, auth_hex);

        auroraos::net::net_sendto(udp_socket_, beacon_payload, strlen(beacon_payload), 0,
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
            int bytes = auroraos::net::net_recvfrom(udp_socket_, recv_buf, sizeof(recv_buf) - 1, 0,
                                                    (struct sockaddr*)&remote_addr, &addr_len);

            if (bytes > 0) {
                recv_buf[bytes] = '\0';

                char ip_str[16]{};
#ifdef AURORA_HOST_TEST
                const uint8_t* raw_ip = reinterpret_cast<const uint8_t*>(&remote_addr.sin_addr);
                snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", static_cast<unsigned>(raw_ip[0]),
                         static_cast<unsigned>(raw_ip[1]), static_cast<unsigned>(raw_ip[2]),
                         static_cast<unsigned>(raw_ip[3]));
#else
                ip4addr_ntoa_r((const ip4_addr_t*)&remote_addr.sin_addr, ip_str, sizeof(ip_str));
#endif

                const uint32_t now_tick = TimerManager::instance().get_current_tick();
                if (bytes >= 4 && static_cast<uint8_t>(recv_buf[0]) == 0x02u &&
                    static_cast<uint8_t>(recv_buf[1]) == 0xFBu) {
                    (void)process_intent_frame(ip_str, reinterpret_cast<const uint8_t*>(recv_buf),
                                               static_cast<size_t>(bytes), now_tick);
                } else {
                    (void)process_beacon_message(ip_str, recv_buf, now_tick);
                }
            }
        }
    }
};

#endif
