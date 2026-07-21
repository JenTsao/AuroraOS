// =============================================================================
// test_lua_vm.cpp — Unit tests for Lua VM Memory Consumption
//
// Strategy: Initialize the KernelHeap with a 64KB static buffer, instantiate
// MiniProgramEngine (which embeds Lua 5.4.6), and measure the difference in
// free memory to evaluate its baseline and execution footprint.
// =============================================================================

#include <gtest/gtest.h>
#include <iostream>
#include <cstdint>
#include <array>

#include "memory.hpp"
#include "../../apps/mini_program_engine.hpp"
#include "../../drivers/display/framebuffer.hpp"
#include "../../drivers/sensor/sensor_framework.hpp"

// Global dependencies required by MiniProgramEngine
FrameBuffer<DISPLAY_WIDTH, DISPLAY_HEIGHT> g_fb;
HeartRateSensor g_health_sensor;

void aurora_get_time(uint32_t& h, uint32_t& m) {
    h = 10;
    m = 9;
}

class LuaVmTest : public ::testing::Test {
protected:
    static constexpr std::size_t kHeapSize = 128 * 1024; // 128 KB heap for Lua tests
    alignas(4) std::array<uint8_t, kHeapSize> heap_storage_{};

    void SetUp() override {
        KernelHeap::instance().init(
            heap_storage_.data(),
            heap_storage_.data() + kHeapSize);
    }
};

TEST_F(LuaVmTest, InitializationMemoryCost) {
    const std::size_t free_before = KernelHeap::instance().get_free_memory();
    
    MiniProgramEngine engine;
    bool init_ok = engine.init();
    ASSERT_TRUE(init_ok);
    
    const std::size_t free_after = KernelHeap::instance().get_free_memory();
    const std::size_t mem_used = free_before - free_after;
    
    std::cout << "\n[Lua Memory] VM Initialization took: " << mem_used << " bytes (" 
              << (mem_used / 1024) << " KB)\n";
    
    // Lua 5.4 with basic libs usually takes 15KB - 35KB
    EXPECT_LT(mem_used, 40000);
    EXPECT_GT(mem_used, 10000);
}

TEST_F(LuaVmTest, ScriptExecutionMemoryCost) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());
    
    const std::size_t free_before = KernelHeap::instance().get_free_memory();
    
    const char* script = R"(
        local a = {}
        for i = 1, 1000 do
            a[i] = i * 2
        end
        return a[500]
    )";
    
    bool load_ok = engine.load_app(script);
    ASSERT_TRUE(load_ok);
    
    const std::size_t free_after = KernelHeap::instance().get_free_memory();
    const std::size_t mem_used = free_before - free_after;
    
    std::cout << "[Lua Memory] Script execution (1000 element array) took: " 
              << mem_used << " bytes (" << (mem_used / 1024) << " KB)\n";
    
    // Arrays in Lua use ~16 bytes per element minimum + table overhead
    EXPECT_GT(mem_used, 16000);
    EXPECT_LT(mem_used, 64000);
}

TEST_F(LuaVmTest, NativeApiBindingMemoryCost) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());
    
    const std::size_t free_before = KernelHeap::instance().get_free_memory();
    
    const char* script = R"(
        -- Use the native bound API
        local hr = aurora.get_heart_rate()
        aurora.fill_rect(0, 0, hr, hr, 0xF800)
    )";
    
    bool load_ok = engine.load_app(script);
    ASSERT_TRUE(load_ok);
    
    const std::size_t free_after = KernelHeap::instance().get_free_memory();
    const std::size_t mem_used = free_before - free_after;
    
    std::cout << "[Lua Memory] Native API script execution took: " 
              << mem_used << " bytes\n\n";
    
    EXPECT_LT(mem_used, 10000);
}

TEST_F(LuaVmTest, CleanupFreesAllMemory) {
    const std::size_t free_before = KernelHeap::instance().get_free_memory();
    
    {
        MiniProgramEngine engine;
        ASSERT_TRUE(engine.init());
        ASSERT_TRUE(engine.load_app("local a = 'hello world'"));
    } // engine destroyed here (calls lua_close)
    
    KernelHeap::instance().defragment();
    const std::size_t free_after = KernelHeap::instance().get_free_memory();
    
    // Verify zero memory leaks after closing VM
    EXPECT_EQ(free_before, free_after);
}
