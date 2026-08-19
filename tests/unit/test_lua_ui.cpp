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
