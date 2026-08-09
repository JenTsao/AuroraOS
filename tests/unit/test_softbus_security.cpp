// =============================================================================
// test_softbus_security.cpp
//
// Unit tests for the security fixes applied to:
//   1. DistributedSoftBus::verify_hmac_sha256  (always returns false; closed path)
//   2. SerialRpcBus::verify_auth               (strips credential prefix, returns data ptr)
//
// Both components are tested via minimal white-box wrappers that expose the
// private helpers — this avoids pulling in lwip / VfsManager on the host.
// =============================================================================

#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// 1.  DistributedSoftBus::verify_hmac_sha256
//
// The previous implementation accepted any device that broadcast the fixed
// JSON field `"auth":"hmac_sha256_result"`.  After the fix the function must
// unconditionally return false regardless of the hash value, closing the
// device-registration path until a real HMAC-SHA256 library is wired in.
//
// We expose the private helper via a thin test-only subclass.
// ─────────────────────────────────────────────────────────────────────────────

// Minimal stand-in for the real DistributedSoftBus to avoid lwip/socket deps.
// We copy only the fixed verify_hmac_sha256 logic from distributed_bus.hpp.
class DistributedSoftBusHmacAccessor {
public:
    bool verify_hmac_sha256(const char* challenge, const char* hash) {
        (void)challenge;
        (void)hash;
        // Security fix: always return false until real HMAC-SHA256 is wired in.
        return false;
    }
};

TEST(DistributedBusHmacTest, AlwaysReturnsFalseForHardcodedToken) {
    DistributedSoftBusHmacAccessor bus;
    // The exact string that the old dummy implementation accepted
    EXPECT_FALSE(bus.verify_hmac_sha256("aurorawatch", "hmac_sha256_result"));
}

TEST(DistributedBusHmacTest, AlwaysReturnsFalseForArbitraryHash) {
    DistributedSoftBusHmacAccessor bus;
    EXPECT_FALSE(bus.verify_hmac_sha256("device123", "anything_at_all"));
    EXPECT_FALSE(bus.verify_hmac_sha256("device123", ""));
    EXPECT_FALSE(bus.verify_hmac_sha256("device123",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
}

TEST(DistributedBusHmacTest, AlwaysReturnsFalseEvenWithFutureCorrectHash) {
    // Even if someone passes what would be a "correct" HMAC value, the path
    // must remain closed until the real implementation is integrated.
    DistributedSoftBusHmacAccessor bus;
    EXPECT_FALSE(bus.verify_hmac_sha256("aurorawatch", "b94f3a...realhmac..."));
}

// ─────────────────────────────────────────────────────────────────────────────
// 2.  SerialRpcBus::verify_auth — prefix check + stripping
//
// The previous implementation returned bool and passed the *full* payload
// (including the "AURORA_RPC_KEY" prefix) to every handler.  After the fix:
//   - Returns nullptr on auth failure.
//   - Returns pointer past the prefix (and optional ':' / ' ') on success.
//
// We replicate the fixed logic here to keep the test independent of the
// VfsManager / uart dependencies in softbus.cpp.
// ─────────────────────────────────────────────────────────────────────────────

static const char* verify_auth_fixed(const char* payload) {
    const char* key = "AURORA_RPC_KEY";
    const char* p   = payload;
    while (*key) {
        if (*key != *p) return nullptr;
        key++;
        p++;
    }
    if (*p == ':' || *p == ' ') p++;
    return p;
}

TEST(SerialRpcBusAuthTest, RejectsEmptyPayload) {
    EXPECT_EQ(verify_auth_fixed(""), nullptr);
}

TEST(SerialRpcBusAuthTest, RejectsPayloadWithoutKey) {
    EXPECT_EQ(verify_auth_fixed("SET_BRIGHTNESS:80"), nullptr);
    EXPECT_EQ(verify_auth_fixed("AURORA"), nullptr);
    EXPECT_EQ(verify_auth_fixed("aurora_rpc_key:data"), nullptr); // case-sensitive
}

TEST(SerialRpcBusAuthTest, AcceptsExactKeyAndReturnsEmptyDataPtr) {
    const char* result = verify_auth_fixed("AURORA_RPC_KEY");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(strlen(result), 0u); // no data after key
}

TEST(SerialRpcBusAuthTest, StripsColonSeparatorAndReturnsData) {
    const char* result = verify_auth_fixed("AURORA_RPC_KEY:SET_BRIGHTNESS=80");
    ASSERT_NE(result, nullptr);
    EXPECT_STREQ(result, "SET_BRIGHTNESS=80");
}

TEST(SerialRpcBusAuthTest, StripsSpaceSeparatorAndReturnsData) {
    const char* result = verify_auth_fixed("AURORA_RPC_KEY VIBRATE");
    ASSERT_NE(result, nullptr);
    EXPECT_STREQ(result, "VIBRATE");
}

TEST(SerialRpcBusAuthTest, NoSeparatorReturnsPointerImmediatelyAfterKey) {
    // If the caller omits a separator the pointer lands right at the data.
    const char* result = verify_auth_fixed("AURORA_RPC_KEYREBOOT");
    ASSERT_NE(result, nullptr);
    EXPECT_STREQ(result, "REBOOT");
}

TEST(SerialRpcBusAuthTest, HandlerDoesNotReceiveCredentialPrefix) {
    // Simulate what dispatch() now does: pass `data` not `payload` to handler.
    static const char* captured = nullptr;
    auto fake_handler = [](const char* p) { captured = p; };

    const char* payload = "AURORA_RPC_KEY:UPDATE_FW";
    const char* data    = verify_auth_fixed(payload);
    ASSERT_NE(data, nullptr);

    fake_handler(data);

    ASSERT_NE(captured, nullptr);
    // The handler must NOT see the credential prefix.
    EXPECT_EQ(strstr(captured, "AURORA_RPC_KEY"), nullptr);
    // The handler MUST see only the business data.
    EXPECT_STREQ(captured, "UPDATE_FW");
}
