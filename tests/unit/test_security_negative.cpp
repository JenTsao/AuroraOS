// =============================================================================
// tests/unit/test_security_negative.cpp
//
// Security negative tests: every case asserts that a privileged operation is
// *denied* and, where state already exists, that the denial is atomic.
//
// The positive paths are covered by test_cspace / test_device_registry /
// test_syscall_ipc.  What matters for a capability kernel is the failure path:
// a rejected escalation that has already destroyed the destination capability
// is a denial-of-service, and a rejected double-delete that decrements a
// refcount is a use-after-free waiting to happen.
//
// Coverage:
//   1. derive/mint/grant escalation leaves the destination untouched
//   2. derive/mint to the source slot is rejected (no phantom slot)
//   3. cap_insert rejects Null capabilities and invalid slots
//   4. replacing a slot releases the old object exactly once
//   5. double delete does not underflow the refcount
//   6. revoke keeps the source and drops only the copies
//   7. MPU SRD mask clamps oversized requests instead of overflowing
//   8. SandboxDescriptor CRC rejects size/base tampering
//   9. SyscallValidator never treats a null task as "no restrictions"
//  10. device operations reject a non-device capability (type confusion)
// =============================================================================

#include <gtest/gtest.h>

#include "../../kernel/task/task.hpp"
#include "../../kernel/core/cspace.hpp"
#include "../../kernel/core/device.hpp"
#include "../../kernel/core/ipc.hpp"
#include "../../kernel/core/syscall_validator.hpp"
#include "../../kernel/mm/mpu.hpp"

using namespace auroraos::kernel;

class SecurityNegativeTest : public ::testing::Test {
protected:
    void SetUp() override {
        Scheduler::instance().init();
    }

    TaskControlBlock* create_task() {
        // Static storage: the scheduler keeps the pointer after the helper
        // returns, so a stack-local buffer would dangle immediately.
        static uint32_t stacks[8][256];
        static size_t next_stack = 0;
        uint32_t* stack = stacks[next_stack % 8];
        ++next_stack;
        return Scheduler::instance().create_task([]() {}, stack, sizeof(stacks[0]));
    }

    static Capability make_cap(CapType type, KernelObject* object, uint32_t rights, uint32_t badge) {
        Capability cap{};
        cap.type = type;
        cap.object = object;
        cap.rights.read = (rights & CAP_RIGHT_READ) != 0;
        cap.rights.write = (rights & CAP_RIGHT_WRITE) != 0;
        cap.rights.grant = (rights & CAP_RIGHT_GRANT) != 0;
        cap.badge = badge;
        return cap;
    }
};

// -----------------------------------------------------------------------------
// 1. Rejected escalation must not touch the destination capability.
//
// cap_derive_internal checks rights *before* cap_delete(dst).  If that order is
// ever inverted, a caller can destroy another capability it does not own just
// by requesting an illegal derive.
// -----------------------------------------------------------------------------
TEST_F(SecurityNegativeTest, DeriveEscalationLeavesDestinationIntact) {
    TaskControlBlock* task = create_task();
    ASSERT_NE(task, nullptr);

    Endpoint source;
    Endpoint destination;

    ASSERT_TRUE(CSpace::cap_insert(task, 0, make_cap(CapType::Endpoint, &source, CAP_RIGHT_READ, 7)));
    ASSERT_TRUE(CSpace::cap_insert(task, 1, make_cap(CapType::Endpoint, &destination,
                                                     CAP_RIGHT_READ | CAP_RIGHT_WRITE, 9)));

    const uint32_t destination_refs = destination.get_ref_count();
    const uint32_t source_refs = source.get_ref_count();

    EXPECT_FALSE(CSpace::cap_derive(task, 0, 1, CAP_RIGHT_READ | CAP_RIGHT_WRITE));

    Capability* dst = CSpace::cap_lookup(task, 1);
    ASSERT_NE(dst, nullptr);
    EXPECT_EQ(dst->type, CapType::Endpoint);
    EXPECT_EQ(dst->object, &destination);
    EXPECT_EQ(dst->badge, 9u);
    EXPECT_TRUE(dst->rights.read);
    EXPECT_TRUE(dst->rights.write);
    EXPECT_EQ(destination.get_ref_count(), destination_refs);
    EXPECT_EQ(source.get_ref_count(), source_refs);
}

TEST_F(SecurityNegativeTest, GrantEscalationLeavesDestinationIntact) {
    TaskControlBlock* source_task = create_task();
    TaskControlBlock* destination_task = create_task();
    ASSERT_NE(source_task, nullptr);
    ASSERT_NE(destination_task, nullptr);

    Endpoint source;
    Endpoint destination;

    ASSERT_TRUE(CSpace::cap_insert(source_task, 0, make_cap(CapType::Endpoint, &source, CAP_RIGHT_READ, 1)));
    ASSERT_TRUE(CSpace::cap_insert(destination_task, 0,
                                   make_cap(CapType::Endpoint, &destination, CAP_RIGHT_READ, 5)));

    const uint32_t destination_refs = destination.get_ref_count();

    // Requesting write authority the source does not hold must fail, and the
    // destination task's existing capability must survive.
    EXPECT_FALSE(CSpace::cap_grant(source_task, destination_task, 0, 0,
                                   CAP_RIGHT_READ | CAP_RIGHT_WRITE, 11));

    Capability* dst = CSpace::cap_lookup(destination_task, 0);
    ASSERT_NE(dst, nullptr);
    EXPECT_EQ(dst->object, &destination);
    EXPECT_EQ(dst->badge, 5u);
    EXPECT_FALSE(dst->rights.write);
    EXPECT_EQ(destination.get_ref_count(), destination_refs);
}

// -----------------------------------------------------------------------------
// 2. src_slot == dst_slot would make derive delete the source before copying
//    from it, leaving an occupied-but-Null "phantom" slot.
// -----------------------------------------------------------------------------
TEST_F(SecurityNegativeTest, DeriveAndMintToSourceSlotRejected) {
    TaskControlBlock* task = create_task();
    ASSERT_NE(task, nullptr);

    Endpoint endpoint;
    ASSERT_TRUE(CSpace::cap_insert(task, 0, make_cap(CapType::Endpoint, &endpoint,
                                                     CAP_RIGHT_READ | CAP_RIGHT_WRITE, 42)));

    EXPECT_FALSE(CSpace::cap_derive(task, 0, 0, CAP_RIGHT_READ));
    EXPECT_FALSE(CSpace::cap_mint(task, 0, 0, CAP_RIGHT_READ, 43));

    Capability* cap = CSpace::cap_lookup(task, 0);
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->type, CapType::Endpoint);
    EXPECT_EQ(cap->object, &endpoint);
    EXPECT_EQ(cap->badge, 42u);
    EXPECT_TRUE(CSpace::is_slot_occupied(task, 0));
}

// -----------------------------------------------------------------------------
// 3. Null capabilities and out-of-range slots must never occupy a slot.
// -----------------------------------------------------------------------------
TEST_F(SecurityNegativeTest, InsertRejectsNullCapabilityAndInvalidSlot) {
    TaskControlBlock* task = create_task();
    ASSERT_NE(task, nullptr);

    Capability null_cap = make_cap(CapType::Null, nullptr, 0, 0);
    EXPECT_FALSE(CSpace::cap_insert(task, 0, null_cap));
    EXPECT_FALSE(CSpace::is_slot_occupied(task, 0));
    EXPECT_EQ(CSpace::cap_lookup(task, 0), nullptr);

    Endpoint endpoint;
    EXPECT_FALSE(CSpace::cap_insert(task, MAX_CSPACE_SLOTS,
                                    make_cap(CapType::Endpoint, &endpoint, CAP_RIGHT_READ, 0)));
}

// -----------------------------------------------------------------------------
// 4. Overwriting an occupied slot releases the previous object exactly once.
// -----------------------------------------------------------------------------
TEST_F(SecurityNegativeTest, InsertReplacesSlotAndReleasesOldObject) {
    TaskControlBlock* task = create_task();
    ASSERT_NE(task, nullptr);

    Endpoint old_endpoint;
    Endpoint new_endpoint;

    ASSERT_TRUE(CSpace::cap_insert(task, 0, make_cap(CapType::Endpoint, &old_endpoint, CAP_RIGHT_READ, 1)));
    EXPECT_EQ(old_endpoint.get_ref_count(), 2u); // object + capability

    ASSERT_TRUE(CSpace::cap_insert(task, 0, make_cap(CapType::Endpoint, &new_endpoint, CAP_RIGHT_READ, 2)));

    EXPECT_EQ(old_endpoint.get_ref_count(), 1u) << "old object must be released on replacement";
    EXPECT_EQ(new_endpoint.get_ref_count(), 2u);

    Capability* cap = CSpace::cap_lookup(task, 0);
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->object, &new_endpoint);
}

// -----------------------------------------------------------------------------
// 5. Deleting an already-Null slot must not release the object a second time.
// -----------------------------------------------------------------------------
TEST_F(SecurityNegativeTest, DoubleDeleteDoesNotUnderflowRefcount) {
    TaskControlBlock* task = create_task();
    ASSERT_NE(task, nullptr);

    Endpoint endpoint;
    ASSERT_TRUE(CSpace::cap_insert(task, 0, make_cap(CapType::Endpoint, &endpoint, CAP_RIGHT_READ, 0)));
    EXPECT_EQ(endpoint.get_ref_count(), 2u);

    EXPECT_TRUE(CSpace::cap_delete(task, 0));
    EXPECT_EQ(endpoint.get_ref_count(), 1u);

    // Second delete sees CapType::Null and must be a no-op for the refcount.
    EXPECT_TRUE(CSpace::cap_delete(task, 0));
    EXPECT_EQ(endpoint.get_ref_count(), 1u);
    EXPECT_FALSE(CSpace::is_slot_occupied(task, 0));
}

// -----------------------------------------------------------------------------
// 6. Revoke keeps the caller's authority and drops every other copy.
// -----------------------------------------------------------------------------
TEST_F(SecurityNegativeTest, RevokePreservesSourceAndDropsCopies) {
    TaskControlBlock* owner = create_task();
    TaskControlBlock* peer = create_task();
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(peer, nullptr);

    Endpoint endpoint;
    ASSERT_TRUE(CSpace::cap_insert(owner, 0, make_cap(CapType::Endpoint, &endpoint,
                                                      CAP_RIGHT_READ | CAP_RIGHT_WRITE, 1)));
    ASSERT_TRUE(CSpace::cap_insert(peer, 0, make_cap(CapType::Endpoint, &endpoint,
                                                     CAP_RIGHT_READ | CAP_RIGHT_WRITE, 1)));
    EXPECT_EQ(endpoint.get_ref_count(), 3u); // object + two capabilities

    EXPECT_TRUE(CSpace::cap_revoke(owner, 0));

    Capability* kept = CSpace::cap_lookup(owner, 0);
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->object, &endpoint);
    EXPECT_EQ(CSpace::cap_lookup(peer, 0), nullptr);
    EXPECT_EQ(endpoint.get_ref_count(), 2u) << "only the source capability may survive";
}

// -----------------------------------------------------------------------------
// 7. SRD mask must clamp, never wrap, when the request exceeds the region.
// -----------------------------------------------------------------------------
TEST_F(SecurityNegativeTest, MpuSrdMaskClampsOversizedRequest) {
    // Region 4096 B, requested 64 KiB: clamp to 7 usable subregions, so only
    // the guard subregion stays disabled.
    EXPECT_EQ(MPU::calculate_stack_srd_mask(65536, 12, true), 0x01u);

    // Zero-byte request still needs one usable subregion (mask 0x7F), not a
    // full-region mask that would make the stack inaccessible.
    EXPECT_EQ(MPU::calculate_stack_srd_mask(0, 12, true), 0x7Fu);
    EXPECT_EQ(MPU::calculate_stack_srd_mask(1, 12, true), 0x7Fu);

    // With the guard disabled, a full-region request disables nothing.
    EXPECT_EQ(MPU::calculate_stack_srd_mask(4096, 12, false), 0x00u);

    // Regions smaller than 256 B have no subregions at all.
    EXPECT_EQ(MPU::calculate_stack_srd_mask(1024, 7, true), 0x00u);
}

// -----------------------------------------------------------------------------
// 8. CRC seal covers size and base, not only the SRD mask.
// -----------------------------------------------------------------------------
TEST_F(SecurityNegativeTest, SandboxDescriptorRejectsSizeAndBaseTampering) {
    SandboxDescriptor desc{};
    desc.stack_base = 0x20001000;
    desc.size_pow2 = 12;
    desc.subregion_disable = 0x01;
    desc.version = 1;
    desc.seal();
    ASSERT_TRUE(desc.is_valid());

    desc.size_pow2 = 13; // widen the region without re-sealing
    EXPECT_FALSE(desc.is_valid());

    desc.size_pow2 = 12;
    desc.seal();
    ASSERT_TRUE(desc.is_valid());

    desc.stack_base += 4096; // move the region without re-sealing
    EXPECT_FALSE(desc.is_valid());
}

// -----------------------------------------------------------------------------
// 9. A null task must not be interpreted as "no address restrictions".
// -----------------------------------------------------------------------------
TEST_F(SecurityNegativeTest, SyscallValidatorRejectsNullTask) {
    uint8_t buffer[16] = {};

    EXPECT_FALSE(SyscallValidator::validate_user_ptr(buffer, sizeof(buffer), nullptr, false));
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(buffer, sizeof(buffer), nullptr, true));
    EXPECT_FALSE(SyscallValidator::validate_user_ptr(nullptr, sizeof(buffer), nullptr, false));
}

// -----------------------------------------------------------------------------
// 10. A capability of the wrong type must not reach a device operation.
// -----------------------------------------------------------------------------
TEST_F(SecurityNegativeTest, DeviceOperationsRejectNonDeviceCapability) {
    TaskControlBlock* task = create_task();
    ASSERT_NE(task, nullptr);

    Endpoint endpoint;
    ASSERT_TRUE(CSpace::cap_insert(task, 0, make_cap(CapType::Endpoint, &endpoint,
                                                     CAP_RIGHT_READ | CAP_RIGHT_WRITE, 0)));

    char buffer[8] = {};
    DeviceRegistry& registry = DeviceRegistry::instance();

    EXPECT_EQ(registry.device_read(task, 0, buffer, sizeof(buffer), 0), -1);
    EXPECT_EQ(registry.device_write(task, 0, buffer, sizeof(buffer), 0), -1);
    EXPECT_EQ(registry.device_ioctl(task, 0, 0, 0), -1);
}
