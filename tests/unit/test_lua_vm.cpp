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

// =============================================================================
// 指令预算（Instruction Budget）—— 防脚本饿死宿主任务
// =============================================================================

// 死循环脚本必须被预算硬性终止，测试本身能在有限时间内结束
TEST_F(LuaVmTest, InstructionBudget_TerminatesInfiniteLoop) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());

    bool ok = engine.load_app("while true do end");
    EXPECT_FALSE(ok); // 以 "instruction budget exceeded" 中止，而非挂死
}

// 正常工作量脚本不受影响
TEST_F(LuaVmTest, InstructionBudget_BoundedWorkloadSucceeds) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());

    bool ok = engine.load_app("local s = 0 for i = 1, 10000 do s = s + i end");
    EXPECT_TRUE(ok);
}

// 预算可配置：小预算拦截中等循环，调大后同脚本通过
TEST_F(LuaVmTest, InstructionBudget_Configurable) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());

    const char* loop = "local s = 0 for i = 1, 200000 do s = s + i end";

    engine.set_instruction_budget(2048);
    EXPECT_FALSE(engine.load_app(loop));

    // 同一脚本，预算充足则通过
    MiniProgramEngine engine2;
    ASSERT_TRUE(engine2.init());
    engine2.set_instruction_budget(50000000);
    EXPECT_TRUE(engine2.load_app(loop));
}

// 预算按入口重置：前一个钩子耗尽预算不影响后续钩子执行
TEST_F(LuaVmTest, InstructionBudget_ResetsBetweenHooks) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());

    ASSERT_TRUE(engine.load_app(
        "function onLoad() local x = 0 for i = 1, 10000000 do x = x + i end end\n"
        "function onShow() return end\n"));

    engine.set_instruction_budget(65536);

    EXPECT_FALSE(engine.call_hook("onLoad"));  // 重循环耗尽预算
    EXPECT_TRUE(engine.call_hook("onShow"));   // 新预算生效，正常执行

    // 不存在的钩子返回 false 且不崩溃
    EXPECT_FALSE(engine.call_hook("on_nonexistent"));
}

// =============================================================================
// 钩子错误熔断器 —— 防劣质脚本每帧刷错拖垮系统
// =============================================================================

TEST_F(LuaVmTest, HookErrorTripsCircuitBreaker) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());

    ASSERT_TRUE(engine.load_app(
        "function onLoad() error('boom') end\n"
        "function onShow() FAULT_MARKER = 1 end\n"));

    // 连续失败达到阈值（3 次）后触发熔断
    EXPECT_FALSE(engine.call_hook("onLoad"));
    EXPECT_FALSE(engine.call_hook("onLoad"));
    EXPECT_FALSE(engine.call_hook("onLoad"));
    EXPECT_TRUE(engine.is_script_faulted());

    // 熔断后所有钩子快速失败，不再进入 VM 执行
    EXPECT_FALSE(engine.call_hook("onShow"));

    // 证明 onShow 的函数体确实未被执行（全局标记未被写入）
    lua_State* L = engine.get_lua_state();
    lua_getglobal(L, "FAULT_MARKER");
    EXPECT_TRUE(lua_isnil(L, -1));
    lua_pop(L, 1);
}

TEST_F(LuaVmTest, SuccessfulHookResetsFailureStreak) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());

    // 交替成功/失败的钩子：连续失败永不达到阈值，不应熔断
    ASSERT_TRUE(engine.load_app(
        "local flip = false\n"
        "function onLoad() flip = not flip; if flip then error('flip-fail') end end\n"));

    for (int i = 0; i < 6; ++i) {
        engine.call_hook("onLoad");
    }
    EXPECT_FALSE(engine.is_script_faulted());
}

TEST_F(LuaVmTest, FaultBlocksLoadingUntilReset) {
    MiniProgramEngine engine;
    ASSERT_TRUE(engine.init());

    ASSERT_TRUE(engine.load_app("function onLoad() error('x') end\n"));
    engine.call_hook("onLoad");
    engine.call_hook("onLoad");
    engine.call_hook("onLoad"); // 第 3 次连续失败 → 熔断
    ASSERT_TRUE(engine.is_script_faulted());

    // 熔断期间拒绝加载（即使脚本本身合法）
    EXPECT_FALSE(engine.load_app("local a = 1"));

    // 宿主显式恢复后一切正常
    engine.reset_script_fault();
    EXPECT_FALSE(engine.is_script_faulted());
    EXPECT_TRUE(engine.load_app("function onTick() return end\n"));
    EXPECT_TRUE(engine.call_hook("onTick"));
}

