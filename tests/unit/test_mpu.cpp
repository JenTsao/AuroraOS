// =============================================================================
// test_mpu.cpp — Unit tests for MPU / Sub-Region Disable (SRD) Stack Protection
//
// Tests:
//   1. MPU Sub-Region Disable calculation for 4KB regions (512B subregions)
//   2. Hardware stack guard sentinel (Subregion 0 disabled: 0x01)
//   3. Fine-grained SRD masks for small tasks (1024B, 1536B, 2048B, 3072B)
//   4. SandboxDescriptor seal and tamper-detection with subregion_disable
//   5. Task creation automatic SRD configuration
// =============================================================================

#include <gtest/gtest.h>
#include "mpu.hpp"
#include "task.hpp"

// ---------------------------------------------------------------------------
// 1. 4KB region with 4096B requested stack: Subregion 0 is disabled as sentinel
// ---------------------------------------------------------------------------
TEST(MpuSrdTest, CalculateMask_4KB_GuardSentinel) {
    // 4096-byte region (size_pow2 = 12). 8 subregions of 512B each.
    // Usable stack: subregions 1..7 (3584B).
    // Sentinel guard: subregion 0 (512B).
    // SRD mask bit 0 must be 1 (disabled), bits 1..7 must be 0 (enabled).
    const uint8_t mask = MPU::calculate_stack_srd_mask(4096, 12, true);
    EXPECT_EQ(mask, 0x01u) << "Subregion 0 must be disabled as stack sentinel";
}

// ---------------------------------------------------------------------------
// 2. Small stacks: Disable lower unused subregions to conserve RAM & bound overflows
// ---------------------------------------------------------------------------
TEST(MpuSrdTest, CalculateMask_SmallStacks) {
    // 1024B stack: needs 2 subregions of 512B (subregions 6 and 7).
    // Subregions 0..5 disabled (0b00111111 = 0x3F).
    const uint8_t mask_1k = MPU::calculate_stack_srd_mask(1024, 12, true);
    EXPECT_EQ(mask_1k, 0x3Fu);

    // 2048B stack: needs 4 subregions of 512B (subregions 4, 5, 6, 7).
    // Subregions 0..3 disabled (0b00001111 = 0x0F).
    const uint8_t mask_2k = MPU::calculate_stack_srd_mask(2048, 12, true);
    EXPECT_EQ(mask_2k, 0x0Fu);

    // 1536B stack: needs 3 subregions of 512B (subregions 5, 6, 7).
    // Subregions 0..4 disabled (0b00011111 = 0x1F).
    const uint8_t mask_1_5k = MPU::calculate_stack_srd_mask(1536, 12, true);
    EXPECT_EQ(mask_1_5k, 0x1Fu);
}

// ---------------------------------------------------------------------------
// 3. Sub-256B regions do not support PMSAv7 SRD
// ---------------------------------------------------------------------------
TEST(MpuSrdTest, SmallRegionsNoSrd) {
    const uint8_t mask = MPU::calculate_stack_srd_mask(128, 7, true);
    EXPECT_EQ(mask, 0u);
}

// ---------------------------------------------------------------------------
// 4. SandboxDescriptor seal and tamper-detection with subregion_disable
// ---------------------------------------------------------------------------
TEST(MpuSrdTest, SandboxDescriptorSealAndVerify) {
    SandboxDescriptor desc{};
    desc.stack_base = 0x20001000;
    desc.size_pow2 = 12;
    desc.subregion_disable = 0x01; // Subregion 0 disabled
    desc.version = 1;
    desc.seal();

    EXPECT_TRUE(desc.is_valid());

    // Tampering with subregion_disable must invalidate the CRC32 seal
    desc.subregion_disable = 0x00;
    EXPECT_FALSE(desc.is_valid());

    // Re-sealing must make it valid again
    desc.seal();
    EXPECT_TRUE(desc.is_valid());
}

// ---------------------------------------------------------------------------
// 5. Task creation automatically sets up SRD guard page for user tasks
// ---------------------------------------------------------------------------
TEST(MpuSrdTest, TaskCreationAutoSrd) {
    alignas(4096) static uint32_t stack_buf[1024]; // 4096 bytes

    TaskControlBlock* tcb = Scheduler::instance().create_task(
        []() {}, stack_buf, sizeof(stack_buf), TaskPriority::Normal, 12, TaskPrivilege::User);

    ASSERT_NE(tcb, nullptr);
    EXPECT_EQ(tcb->memory.subregion_disable, 0x01u) << "User task should default to SRD bit0 guard sentinel";
    EXPECT_EQ(tcb->memory.mpu_sandbox.subregion_disable, 0x01u);
    EXPECT_TRUE(tcb->memory.mpu_sandbox.is_valid());

    Scheduler::instance().free_task(tcb);
}
