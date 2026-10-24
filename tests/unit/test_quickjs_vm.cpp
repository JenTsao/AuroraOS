// =============================================================================
// test_quickjs_vm.cpp — Unit tests for QuickJS MiniProgramEngine & JsHeap
//
// Strategy: Initialize the KernelHeap with a static buffer, instantiate
// JsMiniProgramEngine, and measure the difference in KernelHeap free memory
// to verify 100% memory isolation. Test evaluation, native API bindings,
// lifecycle hooks, instruction budget interrupts, and circuit breaker.
// =============================================================================

#include <gtest/gtest.h>
#include <iostream>
#include <cstdint>
#include <array>

#include "memory.hpp"
#include "../../apps/js_mini_program_engine.hpp"
#include "../../runtime/js_app.hpp"
#include "../../drivers/display/framebuffer.hpp"
#include "../../drivers/sensor/sensor_framework.hpp"
#include "../../ui/ui_config.hpp"

// Global dependencies provided with external linkage in test_lua_vm.cpp
extern AuroraFrameBuffer g_fb;
extern HeartRateSensor g_health_sensor;
extern void aurora_get_time(uint32_t& h, uint32_t& m);

class QuickJsVmTest : public ::testing::Test {
protected:
    static constexpr std::size_t kHeapSize = 128 * 1024;
    alignas(8) std::array<uint8_t, kHeapSize> heap_storage_{};

    void SetUp() override {
        KernelHeap::instance().init(heap_storage_.data(), heap_storage_.data() + kHeapSize);
    }
};

TEST_F(QuickJsVmTest, InitializationMemoryIsolation) {
    const std::size_t kernel_free_before = KernelHeap::instance().get_free_memory();

    JsMiniProgramEngine<131072> engine;
    bool init_ok = engine.init();
    ASSERT_TRUE(init_ok);

    const std::size_t mem_used = engine.get_used_memory();
    const std::size_t kernel_free_after = KernelHeap::instance().get_free_memory();

    std::cout << "\n[QuickJS Memory] VM Initialization took: " << mem_used
              << " bytes (" << (mem_used / 1024) << " KB)\n";

    EXPECT_GT(mem_used, 1000);
    EXPECT_LT(mem_used, 120000);

    // 验证内核堆完全未被触碰（0 污染、0 碎片）
    EXPECT_EQ(kernel_free_before, kernel_free_after);
}

TEST_F(QuickJsVmTest, BasicJavaScriptEvaluation) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* script = R"(
        const arr = [1, 2, 3, 4, 5];
        const doubled = arr.map(x => x * 2);
        const sum = doubled.reduce((acc, v) => acc + v, 0);
        if (sum !== 30) {
            throw new Error("Math verification failed: " + sum);
        }
    )";

    EXPECT_TRUE(engine.load_app(script));
    EXPECT_GT(engine.get_used_memory(), 0);
}

TEST_F(QuickJsVmTest, NativeApiBindings) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* script = R"(
        // 1. aurora.print and console.log
        aurora.print("QuickJS engine initialized in AuroraOS");
        console.log("Console logging works seamlessly");

        // 2. aurora.getHeartRate
        const hr = aurora.getHeartRate();

        // 3. aurora.fillRect
        aurora.fillRect(0, 0, 10, 10, 0xF800);

        // 4. aurora.getTime
        const t = aurora.getTime();
        if (typeof t.hour !== 'number' || typeof t.minute !== 'number') {
            throw new Error("Invalid time object returned");
        }
    )";

    EXPECT_TRUE(engine.load_app(script));
}

TEST_F(QuickJsVmTest, LifecycleHooksTopLevel) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* script = R"(
        var state = "init";
        function onLoad() {
            state = "loaded";
        }
        function onShow() {
            state = "visible";
        }
        function onHide() {
            state = "hidden";
        }
        function onUnload() {
            state = "destroyed";
        }
    )";

    ASSERT_TRUE(engine.load_app(script));

    EXPECT_TRUE(engine.call_hook("onLoad"));
    EXPECT_TRUE(engine.call_hook("onShow"));
    EXPECT_TRUE(engine.call_hook("onHide"));
    EXPECT_TRUE(engine.call_hook("onUnload"));
}

TEST_F(QuickJsVmTest, LifecycleHooksInAppObject) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* script = R"(
        var app = {
            loaded: false,
            onLoad: function() {
                this.loaded = true;
                aurora.print("app.onLoad called");
            },
            onShow: function() {
                aurora.print("app.onShow called");
            }
        };
    )";

    ASSERT_TRUE(engine.load_app(script));
    EXPECT_TRUE(engine.call_hook("onLoad"));
    EXPECT_TRUE(engine.call_hook("onShow"));
}

TEST_F(QuickJsVmTest, InstructionBudgetInterruptsInfiniteLoop) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    // 设置极低指令预算
    engine.set_instruction_budget(5000);

    const char* infinite_loop = R"(
        let count = 0;
        while (true) {
            count++;
        }
    )";

    // 预算耗尽后应当迅速中断并返回 false，绝对不可死锁宿主线程
    bool result = engine.load_app(infinite_loop);
    EXPECT_FALSE(result);
}

TEST_F(QuickJsVmTest, CircuitBreakerTripsOnConsecutiveErrors) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* buggy_script = R"(
        function onLoad() {
            throw new Error("Fatal script defect");
        }
    )";

    ASSERT_TRUE(engine.load_app(buggy_script));
    EXPECT_FALSE(engine.is_script_faulted());

    // 连续调用 3 次抛错钩子
    EXPECT_FALSE(engine.call_hook("onLoad"));
    EXPECT_FALSE(engine.call_hook("onLoad"));
    EXPECT_FALSE(engine.call_hook("onLoad"));

    // 熔断器应当触发
    EXPECT_TRUE(engine.is_script_faulted());

    // 熔断期间后续调用直接拒绝
    EXPECT_FALSE(engine.call_hook("onLoad"));

    // 手动重置后恢复
    engine.reset_script_fault();
    EXPECT_FALSE(engine.is_script_faulted());
}

TEST_F(QuickJsVmTest, JsAppLifecycleIntegration) {
    auroraos::runtime::AppManifest manifest;
    strncpy(manifest.name, "quickjs_demo", sizeof(manifest.name) - 1);
    strncpy(manifest.version, "1.0.0", sizeof(manifest.version) - 1);

    auroraos::runtime::JsApp<131072> app(manifest, "/apps/test.js");
    EXPECT_STREQ(app.get_name(), "quickjs_demo");
    EXPECT_EQ(app.get_state(), auroraos::runtime::AppState::Created);
}

// =============================================================================
// 回归用例：QuickJS 私有堆必须驱动 QuickJS 自身的内存记账与自动 GC
//
// JS_NewRuntime2 把 JSMallocState* 交给宿主分配器，约定由分配器维护
// malloc_size 并拦截 malloc_limit；QuickJS 只在 JS_NewObject 里依据
// malloc_size 与 malloc_gc_threshold 决定是否 JS_RunGC。若宿主回调不记账，
// 自动 GC 永不触发，128KB 池会在短循环里直接 OOM，而不是回收垃圾。
// =============================================================================
TEST_F(QuickJsVmTest, AutomaticGarbageCollectionReclaimsCyclicGarbage) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    // 0 = 不限制指令预算，避免本用例测的是超时而不是内存
    engine.set_instruction_budget(0);

    // 循环引用是 QuickJS 引用计数回收不了、只能靠 JS_RunGC 的形态。
    // 若宿主分配器不向 JSMallocState 记账，或 GC 阈值高于私有池容量，
    // 自动 GC 永不触发，这些垃圾会一直堆积到 OOM。
    const char* cyclic_garbage = R"(
        for (let i = 0; i < 10000; i++) {
            const a = {};
            const b = {};
            a.self = a;
            a.other = b;
            b.back = a;
        }
    )";

    EXPECT_TRUE(engine.load_app(cyclic_garbage));
    // GC 生效时池不会被吃干：仍有可观余量
    EXPECT_GT(engine.get_free_memory(), 0u);
    EXPECT_LT(engine.get_used_memory(), engine.get_total_memory());
}

TEST_F(QuickJsVmTest, HeapExhaustionIsGracefulAndRecoverable) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());
    engine.set_instruction_budget(0);

    // 构造必然吃满私有池的字符串拼接链
    const char* hog = R"(
        let s = "x";
        for (let i = 0; i < 100000; i++) {
            s = s + s;
        }
    )";

    // 内存耗尽必须表现为可捕获的执行失败，而不是宿主崩溃/挂死
    EXPECT_FALSE(engine.load_app(hog));

    // 池被耗尽后引擎仍需可用：释放后小脚本可以正常执行
    EXPECT_TRUE(engine.load_app("var ping = 1 + 1;"));
}

TEST_F(QuickJsVmTest, EngineRecoversAfterBudgetInterruption) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    engine.set_instruction_budget(5000);
    EXPECT_FALSE(engine.load_app("while (true) {}"));

    // 恢复预算后，同一 Runtime/Context 必须能继续正常求值
    engine.set_instruction_budget(500000);
    EXPECT_TRUE(engine.load_app("var recovered = 42;"));
}
