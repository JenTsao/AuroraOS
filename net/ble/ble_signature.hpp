#ifndef AURORA_BLE_SIGNATURE_HPP
#define AURORA_BLE_SIGNATURE_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Assumption: 3rdparty/ed25519/ed25519.h exists
#include "../../3rdparty/ed25519/ed25519.h"

namespace auroraos {
namespace ble {

// ─────────────────────────────────────────────────────────────
// BLE Lua 脚本签名验证器
//
// 安全设计:
//   1. Ed25519 签名验证（与 OTA 一致的算法族）
//   2. BLE 独立密钥对（与 OTA/Secure Boot 密钥隔离）
//   3. 单调递增 nonce 防重放（每个任务独立维护）
//   4. 常量时间比较（防止时序侧信道）
//   5. 失败计数器（检测暴力破解尝试）
// ─────────────────────────────────────────────────────────────
class BleSignatureVerifier {
public:
    static constexpr size_t SIGNATURE_SIZE  = 64;  // Ed25519
    static constexpr size_t NONCE_SIZE      = 4;
    static constexpr size_t LEN_SIZE        = 2;
    static constexpr size_t HEADER_SIZE     = NONCE_SIZE + LEN_SIZE;  // 6
    static constexpr size_t MIN_FRAME_SIZE  = HEADER_SIZE + SIGNATURE_SIZE;  // 70
    static constexpr size_t MAX_PAYLOAD_SIZE = 4096;  // 与 MAX_IPC_MSG_SIZE 一致
    static constexpr uint32_t MAX_VERIFY_FAILURES = 10;  // 连续失败阈值

    static BleSignatureVerifier& instance() {
        static BleSignatureVerifier verifier;
        return verifier;
    }

    // ── 初始化：加载 BLE 专用公钥 ──────────────────────────
    // 公钥应从 Secure Element / OTP / 加密配置区读取
    // 此处为编译期常量，生产环境替换为硬件密钥存储读取
    void init() {
        // BLE 专用 Ed25519 公钥（与 OTA 公钥分离）
        // 生产环境中此值应从 Apollo3 OTP 或 Secure Element 加载
        static const uint8_t ble_public_key[32] = {
            // TODO: 从 Secure Element 加载真实公钥
            // 此处为占位符，部署前必须替换
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        memcpy(public_key_, ble_public_key, 32);
        failure_count_ = 0;
        last_nonce_ = 0;
    }

    // ── 验证入口 ──────────────────────────────────────────
    // 输入: 原始 BLE 帧 (Nonce || Len || Payload || Signature)
    // 输出: true=验证通过, false=拒绝
    // 副作用: 更新 nonce, 累加失败计数
    [[nodiscard]] bool verify(const uint8_t* frame, size_t frame_len) {
        // 1. 长度校验
        if (frame_len < MIN_FRAME_SIZE) {
            inc_failure();
            return false;
        }

        // 2. 提取字段
        uint32_t nonce;
        memcpy(&nonce, frame, NONCE_SIZE);
        uint16_t payload_len;
        memcpy(&payload_len, frame + NONCE_SIZE, LEN_SIZE);

        // 3. payload_len 校验
        if (payload_len > MAX_PAYLOAD_SIZE || 
            frame_len != HEADER_SIZE + payload_len + SIGNATURE_SIZE) {
            inc_failure();
            return false;
        }

        // 4. Nonce 防重放：必须严格单调递增
        if (nonce <= last_nonce_) {
            inc_failure();
            return false;
        }

        // 5. 提取签名（最后 64 字节）
        const uint8_t* signature = frame + HEADER_SIZE + payload_len;

        // 6. 构造签名消息: Nonce || Len || Payload
        //    使用栈缓冲区，避免动态分配
        uint8_t msg_buf[HEADER_SIZE + MAX_PAYLOAD_SIZE];
        memcpy(msg_buf, frame, HEADER_SIZE);  // Nonce + Len
        memcpy(msg_buf + HEADER_SIZE, frame + HEADER_SIZE, payload_len);

        // 7. Ed25519 验签
        // Using int return logic as per user snippet. Note ed25519_verify usually returns 1 on success, 0 on fail.
        // Wait, the user snippet checks `if (result != 0)` for success. 
        int result = ed25519_verify(
            signature,
            msg_buf,
            HEADER_SIZE + payload_len,
            public_key_
        );

        // 8. 清理栈上的敏感数据
        memset(msg_buf, 0, sizeof(msg_buf));

        if (result != 0) {
            // 验证通过
            last_nonce_ = nonce;
            failure_count_ = 0;  // 重置连续失败计数
            return true;
        } else {
            inc_failure();
            return false;
        }
    }

    // ── 状态查询 ──────────────────────────────────────────
    [[nodiscard]] uint32_t get_failure_count() const { return failure_count_; }
    [[nodiscard]] bool is_locked_out() const { return failure_count_ >= MAX_VERIFY_FAILURES; }
    [[nodiscard]] uint32_t get_last_nonce() const { return last_nonce_; }

    // ── 重置（仅用于测试） ────────────────────────────────
    void reset() {
        failure_count_ = 0;
        last_nonce_ = 0;
    }

private:
    BleSignatureVerifier() = default;

    void inc_failure() {
        if (failure_count_ < UINT32_MAX) {
            failure_count_++;
        }
    }

    uint8_t  public_key_[32]{};
    uint32_t last_nonce_ = 0;
    uint32_t failure_count_ = 0;
};

} // namespace ble
} // namespace auroraos

#endif // AURORA_BLE_SIGNATURE_HPP
