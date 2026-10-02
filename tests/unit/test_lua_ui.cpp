#include <gtest/gtest.h>
#include "../../apps/lua_ui_binding.hpp"
#include "../../ui/ui_manager.hpp"
#include "../../ui/view_group.hpp"
#include "../../ui/screen.hpp"
#include "../../ui/screen_navigator.hpp"

class LuaUIBindingTest : public ::testing::Test {
protected:
    void SetUp() override {
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

TEST_F(LuaUIBindingTest, LoadAndInstantiateViews) {
    lua_State* L = luaL_newstate();
    ASSERT_NE(L, nullptr);

    luaopen_aurora_ui(L);

    const char* script = R"(
        local vg = aurora.ui.ViewGroup(0, 0, 100, 100)
        local tv = aurora.ui.TextView(10, 10, "Hello", 65535)
        local arc = aurora.ui.ArcProgress(50, 50, 20, 75, 63488)
        vg:add_child(tv)
        vg:add_child(arc)
        aurora.ui.set_root_view(vg)
    )";

    int result = luaL_dostring(L, script);
    if (result != LUA_OK) {
        printf("Lua error: %s\n", lua_tostring(L, -1));
    }
    EXPECT_EQ(result, LUA_OK);

    UI::ViewGroup* root = UI::UiManager::instance().get_root_view();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->get_width(), 100);

    // 先从 UiManager 解绑，再销毁对象，避免悬空指针（ViewGroup 析构会自动递归释放所有子视图）
    UI::UiManager::instance().set_root_view(nullptr);
    delete root;

    lua_close(L);
}

TEST_F(LuaUIBindingTest, NavigatorAndClick) {
    lua_State* L = luaL_newstate();
    ASSERT_NE(L, nullptr);
    luaopen_aurora_ui(L);

    const char* script = R"(
        local vg = aurora.ui.ViewGroup(0, 0, 100, 100)
        local tv = aurora.ui.TextView(10, 10, "Click Me", 65535)

        tv:set_on_click_listener(function()
            aurora.ui.navigator_pop()
        end)

        vg:add_child(tv)
        aurora.ui.navigator_push(vg)
    )";

    int result = luaL_dostring(L, script);
    if (result != LUA_OK) {
        printf("Lua error: %s\n", lua_tostring(L, -1));
    }
    EXPECT_EQ(result, LUA_OK);

    // After script runs, a Screen with our ViewGroup should be pushed.
    UI::Screen* active = UI::ScreenNavigator::instance().active_screen();
    ASSERT_NE(active, nullptr);

    // Simulate click on the text view (10, 10)
    GestureEvent evt = {GestureType::TAP, 15, 15};
    UI::ScreenNavigator::instance().handle_gesture(evt);

    // Tick to allow pop animation to start
    UI::ScreenNavigator::instance().on_tick(30);

    // Clean up singleton navigator to free LuaCallbackCtx allocations
    UI::ScreenNavigator::instance().clear();

    lua_close(L);
}

// 所有权契约：视图挂载路径 (add_child / set_root_view) 必须移交所有权
// (attached=true)，未挂载视图保持 Lua 持有 (attached=false) 并由 __gc 回收。
TEST_F(LuaUIBindingTest, OwnershipFlagHandoffOnAttach) {
    lua_State* L = luaL_newstate();
    ASSERT_NE(L, nullptr);
    luaopen_aurora_ui(L);

    // 孤儿视图保持 Lua 持有；挂载视图移交宿主
    const char* script = R"(
        orphan = aurora.ui.TextView(0, 0, "orphan", 65535)
        vg = aurora.ui.ViewGroup(0, 0, 100, 100)
        tv = aurora.ui.TextView(1, 1, "child", 65535)
        vg:add_child(tv)
    )";
    ASSERT_EQ(luaL_dostring(L, script), LUA_OK);

    lua_getglobal(L, "orphan");
    auto* orphan_ud = static_cast<ViewUserData*>(lua_touserdata(L, -1));
    ASSERT_NE(orphan_ud, nullptr);
    EXPECT_FALSE(orphan_ud->attached);
    lua_pop(L, 1);

    lua_getglobal(L, "tv");
    auto* tv_ud = static_cast<ViewUserData*>(lua_touserdata(L, -1));
    ASSERT_NE(tv_ud, nullptr);
    EXPECT_TRUE(tv_ud->attached); // add_child 接受后移交宿主
    lua_pop(L, 1);

    lua_getglobal(L, "vg");
    auto* vg_ud = static_cast<ViewUserData*>(lua_touserdata(L, -1));
    ASSERT_NE(vg_ud, nullptr);
    EXPECT_FALSE(vg_ud->attached); // vg 本身尚未挂载
    lua_pop(L, 1);

    ASSERT_EQ(luaL_dostring(L, "aurora.ui.set_root_view(vg)"), LUA_OK);
    EXPECT_TRUE(vg_ud->attached); // set_root_view 接管根视图引用

    // 先关 Lua 态（终结器对 attached 视图不做任何事），
    // 再由 fixture TearDown 销毁宿主持有的根视图树，顺序安全。
    lua_close(L);
}

// 孤儿视图（创建后从未挂载）必须在 GC/lua_close 时被 __gc 回收，
// 而挂载视图即使脚本仍持有 userdata 也不得被重复释放。
TEST_F(LuaUIBindingTest, OrphanViewFreedButAttachedViewSurvivesClose) {
    lua_State* L = luaL_newstate();
    ASSERT_NE(L, nullptr);
    luaopen_aurora_ui(L);

    const char* script = R"(
        vg = aurora.ui.ViewGroup(0, 0, 100, 100)
        tv = aurora.ui.TextView(1, 1, "kept", 65535)
        vg:add_child(tv)
        aurora.ui.set_root_view(vg)
        ghost = aurora.ui.TextView(2, 2, "orphan", 65535)
        ghost = nil -- 丢弃引用：成为孤儿，应被 __gc 回收
    )";
    ASSERT_EQ(luaL_dostring(L, script), LUA_OK);

    // 裸 lua_State 未加载 base 库（无 collectgarbage），从 C 侧触发两轮
    // 完整 GC：第一轮执行 __gc 终结器，第二轮释放终结后的对象。
    lua_gc(L, LUA_GCCOLLECT, 0, 0);
    lua_gc(L, LUA_GCCOLLECT, 0, 0);

    // 丢弃的孤儿视图已被回收（无强引用且 attached=false）；
    // 挂载的 vg/tv 在 lua_close 时不得被 __gc 重复释放（双重释放会
    // 在 ASAN/调试堆下直接报错），随后由 fixture TearDown 统一销毁。
    lua_close(L);
}
