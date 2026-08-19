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
#include "../../ui/ui_config.hpp"

// Global dependencies required by MiniProgramEngine
// Must match the extern in mini_program_engine.hpp: FrameBuffer<DISPLAY_WIDTH, AURORA_FB_CHUNK_HEIGHT>
FrameBuffer<DISPLAY_WIDTH, AURORA_FB_CHUNK_HEIGHT> g_fb;
HeartRateSensor g_health_sensor;

void aurora_get_time(uint32_t& h, uint32_t& m) {
    h = 10;
    m = 9;
}

class LuaVmTest : public ::testing::Test {
protected:
    static constexpr std::size_t kHeapSize = 128 * 1024; // 128 KB heap for kernel tests
    alignas(8) std::array<uint8_t, kHeapSize> heap_storage_{};

    void SetUp() override {
        KernelHeap::instance().init(heap_storage_.data(), heap_storage_.data() + kHeapSize);
    }
};

TEST_F(LuaVmTest, InitializationMemoryCost) {
    const std::size_t kernel_free_before = KernelHeap::instance().get_free_memory();

    MiniProgramEngine engine;
    bool init_ok = engine.init();
    ASSERT_TRUE(init_ok);

    const std::size_t mem_used = engine.get_used_memory();
    const std::size_t kernel_free_after = KernelHeap::instance().get_free_memory();

    std::cout << "\n[Lua Memory] VM Initialization took: " << mem_used << " bytes (" << (mem_used / 1024) << " KB)\n";

    // Lua 5.4 with basic libs inside LuaHeap takes ~10KB - 30KB
    EXPECT_LT(mem_used, 32000);
    EXPECT_GT(mem_used, 5000);

    // Verify KernelHeap is completely untouched (zero kernel pollution/fragmentation!)
    EXPECT_EQ(kernel_free_before, kernel_free_after);
}

TEST_F(LuaVmTest, ScriptExecutionMemoryCost) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());

    const std::size_t kernel_free_before = KernelHeap::instance().get_free_memory();
    const std::size_t used_before = engine.get_used_memory();

    const char* script = R"(
        local a = {}
        for i = 1, 300 do
            a[i] = i * 2
        end
        return a[150]
    )";

    bool load_ok = engine.load_app(script);
    if (!load_ok) {
        std::cout << "[Lua Error] " << lua_tostring(engine.get_lua_state(), -1) << std::endl;
    }
    ASSERT_TRUE(load_ok);

    const std::size_t used_after = engine.get_used_memory();
    const std::size_t mem_used = used_after - used_before;
    const std::size_t kernel_free_after = KernelHeap::instance().get_free_memory();

    std::cout << "[Lua Memory] Script execution took: " << mem_used << " bytes ("
              << (mem_used / 1024) << " KB)\n";

    // Allocations occurred in LuaHeap
    EXPECT_GT(mem_used, 2000);
    EXPECT_LT(mem_used, 30000);

    // KernelHeap remained completely untouched
    EXPECT_EQ(kernel_free_before, kernel_free_after);
}

TEST_F(LuaVmTest, NativeApiBindingMemoryCost) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());

    const std::size_t kernel_free_before = KernelHeap::instance().get_free_memory();
    const std::size_t used_before = engine.get_used_memory();

    const char* script = R"(
        -- Use the native bound API
        local hr = aurora.get_heart_rate()
        aurora.fill_rect(0, 0, hr, hr, 0xF800)
    )";

    bool load_ok = engine.load_app(script);
    ASSERT_TRUE(load_ok);

    const std::size_t used_after = engine.get_used_memory();
    const std::size_t mem_used = used_after - used_before;
    const std::size_t kernel_free_after = KernelHeap::instance().get_free_memory();

    std::cout << "[Lua Memory] Native API script execution took: " << mem_used << " bytes\n\n";

    EXPECT_LT(mem_used, 10000);
    EXPECT_EQ(kernel_free_before, kernel_free_after);
}

TEST_F(LuaVmTest, CleanupFreesAllMemory) {
    const std::size_t kernel_free_before = KernelHeap::instance().get_free_memory();

    {
        MiniProgramEngine engine;
        ASSERT_TRUE(engine.init());
        ASSERT_TRUE(engine.load_app("local a = 'hello world'"));
        EXPECT_GT(engine.get_used_memory(), 0u);
    } // engine destroyed here (calls lua_close and resets private pool)

    const std::size_t kernel_free_after = KernelHeap::instance().get_free_memory();

    // Verify zero kernel heap leaks
    EXPECT_EQ(kernel_free_before, kernel_free_after);
}

TEST_F(LuaVmTest, LuaHeapDirectOperations) {
    LuaHeap<16384> heap;
    EXPECT_EQ(heap.get_used_memory(), 0u);
    EXPECT_GT(heap.get_free_memory(), 15000u);

    void* p1 = heap.allocate(128);
    ASSERT_NE(p1, nullptr);
    EXPECT_GE(heap.get_used_memory(), 128u);

    // Reallocate (expand)
    void* p2 = heap.reallocate(p1, 128, 256);
    ASSERT_NE(p2, nullptr);

    heap.deallocate(p2);
    EXPECT_EQ(heap.get_used_memory(), 0u);

    heap.reset();
    EXPECT_EQ(heap.get_used_memory(), 0u);
}

