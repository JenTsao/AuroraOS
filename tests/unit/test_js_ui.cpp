// =============================================================================
// tests/unit/test_js_ui.cpp
//
// QuickJS UI 绑定层 (apps/js_ui_binding.hpp) 与 DynamicJsWatchFaceScreen 单元测试
// 覆盖：
//   - 基础控件树构建与挂载 (ViewGroup / TextView / ArcProgress)
//   - ScreenNavigator 页面压栈、点击回调触发与出栈
//   - 所有权移交契约 (attached 标志) 与孤儿视图 GC 自动回收
//   - 孤儿视图自捕获闭包环路 (View -> Callback -> View) 的 gc_mark 破环回收
//   - TextView 动态拼接临时字符串持久化（防 JS_FreeCString 后悬空指针）
//   - 扩展控件族绑定 (Button / ProgressBar / SwitchView / SliderView / ScrollView)
//   - 安全负向测试：类型混淆拦截 (Type Confusion) 与二次挂载拦截 (防 Double Free)
//   - UI 回调中的指令预算超时中断与连续异常熔断器保护
//   - DynamicJsWatchFaceScreen VFS 加载与析构期完整归还内核堆
// =============================================================================

#include <gtest/gtest.h>
#include <array>
#include <cstring>

#include "../../apps/js_mini_program_engine.hpp"
#include "../../apps/js_ui_binding.hpp"
#include "../../apps/watch/screens/dynamic_js_watch_face_screen.hpp"
#include "../../kernel/mm/memory.hpp"
#include "../../ui/screen.hpp"
#include "../../ui/screen_navigator.hpp"
#include "../../ui/ui_manager.hpp"
#include "../../ui/view_group.hpp"
#include "../../vfs/ramfs.hpp"
#include "../../vfs/vfs.hpp"

class JsUIBindingTest : public ::testing::Test {
protected:
    static constexpr size_t kKernelHeapSize = 256 * 1024;
    alignas(8) std::array<uint8_t, kKernelHeapSize> mock_heap_{};

    void SetUp() override {
        KernelHeap::instance().init(mock_heap_.data(), mock_heap_.data() + mock_heap_.size());
        UI::ScreenNavigator::instance().clear();
        UI::UiManager::instance().set_root_view(nullptr);
    }

    void TearDown() override {
        UI::ScreenNavigator::instance().clear();
        UI::ViewGroup* root = UI::UiManager::instance().get_root_view();
        UI::UiManager::instance().set_root_view(nullptr);
        if (root) {
            delete root;
        }
    }
};

TEST_F(JsUIBindingTest, LoadAndInstantiateViews) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* script = R"(
        const vg = aurora.ui.ViewGroup(0, 0, 120, 140);
        const tv = aurora.ui.TextView(10, 10, "Hello JS", 0xFFFF);
        const arc = aurora.ui.ArcProgress(50, 50, 20, 75, 0xF800);
        vg.add_child(tv);
        vg.addChild(arc);
        aurora.ui.set_root_view(vg);
    )";

    EXPECT_TRUE(engine.load_app(script));

    UI::ViewGroup* root = UI::UiManager::instance().get_root_view();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->get_width(), 120);
    EXPECT_EQ(root->get_height(), 140);
    EXPECT_EQ(root->get_child_count(), 2);

    UI::UiManager::instance().set_root_view(nullptr);
    delete root;
}

TEST_F(JsUIBindingTest, NavigatorAndClick) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* script = R"(
        var clickCount = 0;
        const vg = aurora.ui.ViewGroup(0, 0, 100, 100);
        const tv = aurora.ui.TextView(10, 10, "Click Me", 0xFFFF);

        tv.set_on_click_listener(function() {
            clickCount++;
            aurora.ui.navigator_pop();
        });

        vg.add_child(tv);
        aurora.ui.navigator_push(vg);
    )";

    ASSERT_TRUE(engine.load_app(script));

    UI::Screen* active = UI::ScreenNavigator::instance().active_screen();
    ASSERT_NE(active, nullptr);

    GestureEvent evt = {GestureType::TAP, 15, 15};
    EXPECT_TRUE(UI::ScreenNavigator::instance().handle_gesture(evt));

    UI::ScreenNavigator::instance().on_tick(30);

    JSContext* ctx = engine.get_context();
    JSValue global_obj = JS_GetGlobalObject(ctx);
    JSValue count_val = JS_GetPropertyStr(ctx, global_obj, "clickCount");
    int32_t clicks = 0;
    EXPECT_EQ(JS_ToInt32(ctx, &clicks, count_val), 0);
    EXPECT_EQ(clicks, 1);
    JS_FreeValue(ctx, count_val);
    JS_FreeValue(ctx, global_obj);

    UI::ScreenNavigator::instance().clear();
}

TEST_F(JsUIBindingTest, OwnershipFlagHandoffOnAttach) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* script = R"(
        var orphan = aurora.ui.TextView(0, 0, "orphan", 0xFFFF);
        var vg = aurora.ui.ViewGroup(0, 0, 100, 100);
        var tv = aurora.ui.TextView(1, 1, "child", 0xFFFF);
        vg.add_child(tv);
    )";
    ASSERT_TRUE(engine.load_app(script));

    JSContext* ctx = engine.get_context();
    JSValue global_obj = JS_GetGlobalObject(ctx);

    JSValue orphan_val = JS_GetPropertyStr(ctx, global_obj, "orphan");
    JsViewUserData* orphan_ud = js_to_view_ud(ctx, orphan_val);
    ASSERT_NE(orphan_ud, nullptr);
    EXPECT_FALSE(orphan_ud->attached);
    JS_FreeValue(ctx, orphan_val);

    JSValue tv_val = JS_GetPropertyStr(ctx, global_obj, "tv");
    JsViewUserData* tv_ud = js_to_view_ud(ctx, tv_val);
    ASSERT_NE(tv_ud, nullptr);
    EXPECT_TRUE(tv_ud->attached);
    JS_FreeValue(ctx, tv_val);

    JSValue vg_val = JS_GetPropertyStr(ctx, global_obj, "vg");
    JsViewUserData* vg_ud = js_to_view_ud(ctx, vg_val);
    ASSERT_NE(vg_ud, nullptr);
    EXPECT_FALSE(vg_ud->attached);

    ASSERT_TRUE(engine.load_app("aurora.ui.set_root_view(vg);"));
    EXPECT_TRUE(vg_ud->attached);

    JS_FreeValue(ctx, vg_val);
    JS_FreeValue(ctx, global_obj);

    // 先销毁 JS 引擎（验证 attached 视图不会被 finalizer 二次释放），
    // 再由 TearDown 释放 UiManager 持有的根视图。
    engine.teardown();
}

TEST_F(JsUIBindingTest, OrphanViewFreedButAttachedViewSurvivesClose) {
    const size_t free_before = KernelHeap::instance().get_free_memory();

    {
        JsMiniProgramEngine<131072> engine;
        ASSERT_TRUE(engine.init());

        const char* script = R"(
            var vg = aurora.ui.ViewGroup(0, 0, 100, 100);
            var tv = aurora.ui.TextView(1, 1, "kept", 0xFFFF);
            vg.add_child(tv);
            aurora.ui.set_root_view(vg);
            var ghost = aurora.ui.TextView(2, 2, "orphan", 0xFFFF);
            ghost = null;
        )";
        ASSERT_TRUE(engine.load_app(script));

        JS_RunGC(engine.get_runtime());
    }

    // 此时 engine 已析构：孤儿视图已被 finalizer 释放；挂载的 vg/tv 仍归 UiManager 持有
    UI::ViewGroup* root = UI::UiManager::instance().get_root_view();
    ASSERT_NE(root, nullptr);
    UI::UiManager::instance().set_root_view(nullptr);
    delete root;

    // 全部 C++ 视图归还后，内核堆空闲量必须与初始完全一致
    EXPECT_EQ(KernelHeap::instance().get_free_memory(), free_before);
}

TEST_F(JsUIBindingTest, OrphanViewWithSelfCapturingClickClosureReclaimedByCycleGC) {
    const size_t free_before = KernelHeap::instance().get_free_memory();

    {
        JsMiniProgramEngine<131072> engine;
        ASSERT_TRUE(engine.init());

        // 构造典型的 [JS View -> C++ View -> JsCallbackCtx::fn -> JS View] 跨语言环路
        const char* script = R"(
            (function() {
                let ghost = aurora.ui.TextView(5, 5, "cyclic", 0xFFFF);
                ghost.set_on_click_listener(function() {
                    ghost.set_text("clicked");
                });
            })();
        )";
        ASSERT_TRUE(engine.load_app(script));

        // 触发 QuickJS 环路回收：js_view_gc_mark 标出未挂载视图持有的回调，破环释放
        JS_RunGC(engine.get_runtime());
        EXPECT_EQ(KernelHeap::instance().get_free_memory(), free_before);
    }

    EXPECT_EQ(KernelHeap::instance().get_free_memory(), free_before);
}

TEST_F(JsUIBindingTest, TemporaryStringInTextViewDoesNotDangle) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* script = R"(
        const bpm = 75;
        const vg = aurora.ui.ViewGroup(0, 0, 192, 490);
        const tv = aurora.ui.TextView(10, 20, "HR: " + bpm + " BPM", 0xF800, 0, 2, aurora.ui.ALIGN_CENTER);
        vg.add_child(tv);
        tv.set_text("NOW: " + (bpm + 5) + " BPM");
        aurora.ui.set_root_view(vg);
    )";
    ASSERT_TRUE(engine.load_app(script));

    // 强制 GC 回收所有临时 JSString
    JS_RunGC(engine.get_runtime());

    UI::ViewGroup* root = UI::UiManager::instance().get_root_view();
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->get_child_count(), 1);
    auto* tv = static_cast<UI::TextView*>(root->get_child(0));
    ASSERT_NE(tv, nullptr);
    EXPECT_STREQ(tv->get_text(), "NOW: 80 BPM");
    EXPECT_EQ(tv->get_text_align(), UI::TextAlign::CENTER);
}

TEST_F(JsUIBindingTest, ExtendedWidgetsFullSuite) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* script = R"(
        var switchState = false;
        var sliderVal = 0;
        var btnClicked = false;

        const root = aurora.ui.ViewGroup(0, 0, 192, 490);

        const btn = aurora.ui.Button(10, 10, 80, 30, 0x07E0, 0x0500, 6);
        btn.setOnClickListener(function() {
            btnClicked = true;
        });
        root.addChild(btn);

        const bar = aurora.ui.ProgressBar(10, 50, 100, 12, 0, 200, 50);
        bar.setProgress(150);
        if (bar.getPercentage() !== 75) {
            throw new Error("ProgressBar percentage mismatch");
        }
        root.addChild(bar);

        const sw = aurora.ui.SwitchView(10, 80, 50, 28, false);
        sw.setOnCheckedChangedListener(function(checked) {
            switchState = checked;
        });
        root.addChild(sw);

        const slider = aurora.ui.SliderView(10, 120, 100, 24, 0, 100, 20);
        slider.setOnValueChangedListener(function(val) {
            sliderVal = val;
        });
        slider.setValue(65);
        root.addChild(slider);

        const scroll = aurora.ui.ScrollView(0, 200, 192, 100);
        scroll.setContentSize(192, 300);
        scroll.scrollBy(0, 45);
        if (scroll.getScrollY() !== 45) {
            throw new Error("ScrollView scroll_y mismatch");
        }
        root.addChild(scroll);

        aurora.ui.setRootView(root);
    )";

    ASSERT_TRUE(engine.load_app(script));

    // 点击按钮 (20, 20) 与开关 (20, 90)
    GestureEvent tap_btn = {GestureType::TAP, 20, 20};
    EXPECT_TRUE(UI::UiManager::instance().dispatch_gesture(tap_btn));

    GestureEvent tap_sw = {GestureType::TAP, 20, 90};
    EXPECT_TRUE(UI::UiManager::instance().dispatch_gesture(tap_sw));

    JSContext* ctx = engine.get_context();
    JSValue global_obj = JS_GetGlobalObject(ctx);

    JSValue btn_val = JS_GetPropertyStr(ctx, global_obj, "btnClicked");
    EXPECT_EQ(JS_ToBool(ctx, btn_val), 1);
    JS_FreeValue(ctx, btn_val);

    JSValue sw_val = JS_GetPropertyStr(ctx, global_obj, "switchState");
    EXPECT_EQ(JS_ToBool(ctx, sw_val), 1);
    JS_FreeValue(ctx, sw_val);

    JSValue sl_val = JS_GetPropertyStr(ctx, global_obj, "sliderVal");
    int32_t slider_v = 0;
    EXPECT_EQ(JS_ToInt32(ctx, &slider_v, sl_val), 0);
    EXPECT_EQ(slider_v, 65);
    JS_FreeValue(ctx, sl_val);

    JS_FreeValue(ctx, global_obj);
}

TEST_F(JsUIBindingTest, TypeConfusionAndDoubleAttachRejected) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    // 1. 在叶子控件 TextView 上调用容器方法 add_child 必须抛 TypeError
    EXPECT_FALSE(engine.load_app(R"(
        const tv1 = aurora.ui.TextView(0, 0, "a", 0xFFFF);
        const tv2 = aurora.ui.TextView(0, 0, "b", 0xFFFF);
        tv1.add_child(tv2);
    )"));

    // 2. 在容器 ViewGroup 上调用 TextView 专属方法 set_text 必须抛 TypeError
    EXPECT_FALSE(engine.load_app(R"(
        const vg = aurora.ui.ViewGroup(0, 0, 50, 50);
        vg.set_text("invalid");
    )"));

    // 3. 同一子控件重复 add_child 必须抛 TypeError（防止 ~ViewGroup 双重释放）
    engine.reset_script_fault();
    EXPECT_FALSE(engine.load_app(R"(
        const vg = aurora.ui.ViewGroup(0, 0, 50, 50);
        const tv = aurora.ui.TextView(0, 0, "once", 0xFFFF);
        vg.add_child(tv);
        vg.add_child(tv);
    )"));

    // 4. 容器将自身添加为子控件必须抛 TypeError（防止自环递归）
    engine.reset_script_fault();
    EXPECT_FALSE(engine.load_app(R"(
        const vg = aurora.ui.ViewGroup(0, 0, 50, 50);
        vg.add_child(vg);
    )"));
}

TEST_F(JsUIBindingTest, CallbackInstructionBudgetAndCircuitBreaker) {
    JsMiniProgramEngine<131072> engine;
    ASSERT_TRUE(engine.init());

    const char* script = R"(
        const vg = aurora.ui.ViewGroup(0, 0, 100, 100);
        const btn = aurora.ui.Button(0, 0, 100, 100, 0x07E0, 0x0500);
        btn.set_on_click_listener(function() {
            while (true) {}
        });
        vg.add_child(btn);
        aurora.ui.set_root_view(vg);
    )";
    ASSERT_TRUE(engine.load_app(script));

    // 调低预算：点击回调中的死循环必须被中断且累计 3 次后触发熔断
    engine.set_instruction_budget(4096);
    GestureEvent tap = {GestureType::TAP, 20, 20};

    UI::UiManager::instance().dispatch_gesture(tap);
    EXPECT_FALSE(engine.is_script_faulted());

    UI::UiManager::instance().dispatch_gesture(tap);
    EXPECT_FALSE(engine.is_script_faulted());

    UI::UiManager::instance().dispatch_gesture(tap);
    EXPECT_TRUE(engine.is_script_faulted());
}

TEST_F(JsUIBindingTest, DynamicJsWatchFaceScreenLifecycleAndHeapReclamation) {
    VfsManager::instance().init();
    auto* ramfs = new RamFile(4096);
    VfsManager::instance().mount("/lfs", ramfs);

    const char* js_wf_script = R"(
        var tickCount = 0;
        function create_ui() {
            const vg = aurora.ui.ViewGroup(0, 0, 192, 490);
            const tv = aurora.ui.TextView(20, 100, "JS Watch", 0xFFFF, 0, 3);
            const orphan = aurora.ui.TextView(0, 0, "unattached", 0xFFFF);
            tv.set_on_click_listener(function() {
                tv.set_text("Tapped");
            });
            vg.add_child(tv);
            return vg;
        }
        function on_create() {
            aurora.print("JS WatchFace created");
        }
        function on_tick() {
            tickCount++;
        }
    )";

    int fd = VfsManager::instance().open("/lfs/wf_demo.js", O_CREAT | O_WRONLY);
    ASSERT_GE(fd, 0);
    VfsManager::instance().write(fd, js_wf_script, strlen(js_wf_script));
    VfsManager::instance().close(fd);

    const size_t free_before = KernelHeap::instance().get_free_memory();
    {
        aurora::watch::DynamicJsWatchFaceScreen<131072> screen("/lfs/wf_demo.js");
        screen.on_create();
        screen.on_show();
        screen.on_tick(16);
        EXPECT_EQ(screen.get_child_count(), 1);

        GestureEvent tap = {GestureType::TAP, 30, 105};
        EXPECT_TRUE(screen.handle_gesture(tap));
        screen.on_hide();
    }
    // 挂载视图、回调上下文与未挂载孤儿视图必须全部归还内核堆
    EXPECT_EQ(KernelHeap::instance().get_free_memory(), free_before);

    delete ramfs;
}
