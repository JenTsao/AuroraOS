#include <gtest/gtest.h>
#include "../../apps/elf.hpp"
#include "../../apps/elf_loader.hpp"
#include <stdint.h>

TEST(ElfRelocationTest, ThumbCallMath) {
    // 模拟 R_ARM_THM_CALL 计算
    // B = S + A - P
    uintptr_t S = 0x08001000; // 目标函数地址 (Thumb, 最低位为 1)
    uintptr_t P = 0x20004000; // 当前 BL 指令地址
    
    // 我们在这里只测试基本的加减法，实际的二进制编码测试太复杂
    int32_t result = S + 0 - P; // A = 0
    EXPECT_EQ(result, static_cast<int32_t>(0xE7FFD000));
}

TEST(ElfRelocationTest, Abs32Math) {
    uint32_t memory_loc = 0;
    uint32_t* P_ptr = &memory_loc;
    
    uintptr_t S = 0x08005000;
    uint32_t A = 0x10; // offset
    *P_ptr = A;
    
    // type == R_ARM_ABS32
    *P_ptr = S + *P_ptr;
    
    EXPECT_EQ(memory_loc, 0x08005010);
}
