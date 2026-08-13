#include "ble_signature.hpp"

namespace auroraos {
namespace ble {

BleSignatureVerifier& BleSignatureVerifier::instance() {
    static BleSignatureVerifier verifier;
    return verifier;
}

void BleSignatureVerifier::init(const uint8_t* override_pub_key) {
    if (override_pub_key) {
        memcpy(public_key_, override_pub_key, 32);
    } else {
#ifdef AURORA_HOST_TEST
        memset(public_key_, 0x00, 32);
#else
        // 生产环境中此值从 Apollo3 OTP 或 Secure Element 加载
        // 与 OTA(0x400E0000) 分离，这里取 0x400E0020
        const uint8_t* hardware_key = reinterpret_cast<const uint8_t*>(0x400E0020);
        memcpy(public_key_, hardware_key, 32);
#endif
    }
    failure_count_ = 0;
    last_nonce_ = 0;
}

bool BleSignatureVerifier::verify(const uint8_t* frame, size_t frame_len) {
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
    if (payload_len > MAX_PAYLOAD_SIZE || frame_len != HEADER_SIZE + payload_len + SIGNATURE_SIZE) {
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
    memcpy(msg_buf, frame, HEADER_SIZE); // Nonce + Len
    memcpy(msg_buf + HEADER_SIZE, frame + HEADER_SIZE, payload_len);

    // 7. Ed25519 验签
    int result = ed25519_verify(signature, msg_buf, HEADER_SIZE + payload_len, public_key_);

    // 8. 清理栈上的敏感数据
    memset(msg_buf, 0, sizeof(msg_buf));

    if (result != 0) {
        // 验证通过
        last_nonce_ = nonce;
        failure_count_ = 0; // 重置连续失败计数
        return true;
    } else {
        inc_failure();
        return false;
    }
}

uint32_t BleSignatureVerifier::get_failure_count() const {
    return failure_count_;
}

bool BleSignatureVerifier::is_locked_out() const {
    return failure_count_ >= MAX_VERIFY_FAILURES;
}

uint32_t BleSignatureVerifier::get_last_nonce() const {
    return last_nonce_;
}

void BleSignatureVerifier::reset() {
    failure_count_ = 0;
    last_nonce_ = 0;
}

void BleSignatureVerifier::inc_failure() {
    if (failure_count_ < UINT32_MAX) {
        failure_count_++;
    }
}

} // namespace ble
} // namespace auroraos
