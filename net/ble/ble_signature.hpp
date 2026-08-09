#ifndef AURORA_BLE_SIGNATURE_HPP
#define AURORA_BLE_SIGNATURE_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Assumption: 3rdparty/ed25519/ed25519.h exists
#include "../../3rdparty/ed25519/ed25519.h"

namespace auroraos {
namespace ble {

class BleSignatureVerifier {
public:
    static constexpr size_t SIGNATURE_SIZE  = 64;  // Ed25519
    static constexpr size_t NONCE_SIZE      = 4;
    static constexpr size_t LEN_SIZE        = 2;
    static constexpr size_t HEADER_SIZE     = NONCE_SIZE + LEN_SIZE;  // 6
    static constexpr size_t MIN_FRAME_SIZE  = HEADER_SIZE + SIGNATURE_SIZE;  // 70
    static constexpr size_t MAX_PAYLOAD_SIZE = 4096;  // 与 MAX_IPC_MSG_SIZE 一致
    static constexpr uint32_t MAX_VERIFY_FAILURES = 10;  // 连续失败阈值

    static BleSignatureVerifier& instance();

    void init(const uint8_t* override_pub_key = nullptr);

    // ── 验证入口 ──────────────────────────────────────────
    [[nodiscard]] bool verify(const uint8_t* frame, size_t frame_len);

    // ── 状态查询 ──────────────────────────────────────────
    [[nodiscard]] uint32_t get_failure_count() const;
    [[nodiscard]] bool is_locked_out() const;
    [[nodiscard]] uint32_t get_last_nonce() const;

    // ── 重置（仅用于测试） ────────────────────────────────
    void reset();

private:
    BleSignatureVerifier() = default;

    void inc_failure();

    uint8_t  public_key_[32]{};
    uint32_t last_nonce_ = 0;
    uint32_t failure_count_ = 0;
};

} // namespace ble
} // namespace auroraos

#endif // AURORA_BLE_SIGNATURE_HPP
