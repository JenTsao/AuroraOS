#include <gtest/gtest.h>
#include "../../apps/watch/screens/dynamic_watch_face_screen.hpp"
#include "../../kernel/mm/memory.hpp"
#include "../../vfs/vfs.hpp"
#include "../../vfs/ramfs.hpp"

using namespace aurora;
using namespace aurora::watch;

// aurora_get_time is provided by test_lua_vm.cpp

// Test fixture for Lua dynamic watch faces
class DynamicWatchFaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Init memory
        KernelHeap::instance().init(&mock_heap_[0], &mock_heap_[sizeof(mock_heap_)]);
        VfsManager::instance().init();

        // Mount a RAMFS to simulate LittleFS
        ramfs_ = new RamFile(4096);
        VfsManager::instance().mount("/lfs", ramfs_);

        // Write a valid Lua watch face script to VFS
        const char* lua_script = "function create_ui()\n"
                                 "    local vg = aurora.ui.ViewGroup(0, 0, 192, 490)\n"
                                 "    local tv = aurora.ui.TextView(20, 100, \"Lua Time\", 65535, 0, 4)\n"
                                 "    vg:add_child(tv)\n"
                                 "    return vg\n"
                                 "end\n"
                                 "function on_create()\n"
                                 "    aurora.print(\"WF created\")\n"
                                 "end\n";

        int fd = VfsManager::instance().open("/lfs/test_wf.lua", O_CREAT | O_WRONLY);
        if (fd >= 0) {
            VfsManager::instance().write(fd, lua_script, strlen(lua_script));
            VfsManager::instance().close(fd);
        }
    }

    void TearDown() override {
        delete ramfs_;
    }

    RamFile* ramfs_;
    alignas(8) uint8_t mock_heap_[256 * 1024];
};

TEST_F(DynamicWatchFaceTest, LoadsAndCreatesLuaUI) {
    DynamicWatchFaceScreen screen("/lfs/test_wf.lua");

    // on_create will load the Lua script, run create_ui(), and add the returned ViewGroup as a child.
    screen.on_create();

    // The screen itself is a ViewGroup. If the Lua script successfully returned a ViewGroup,
    // it will be added, but ViewGroup has no getter. We just ensure it doesn't crash.
    screen.on_show();
}

TEST_F(DynamicWatchFaceTest, FailsGracefullyOnInvalidFile) {
    DynamicWatchFaceScreen screen("/lfs/non_existent.lua");

    // Shouldn't crash
    screen.on_create();
}

// ========================================================
// 生命周期/泄漏验证：挂载到 Screen 的 Lua 视图树必须随 Screen
// 析构完整归还内核堆。析构顺序由 C++ 保证：成员 engine_ (lua_close，
// __gc 终结器在此运行并跳过 attached 视图) 先于基类 ViewGroup 销毁子视图。
// ========================================================
TEST_F(DynamicWatchFaceTest, ScreenDestructionReclaimsAttachedViews) {
    const size_t free_before = KernelHeap::instance().get_free_memory();
    {
        DynamicWatchFaceScreen screen("/lfs/test_wf.lua");
        screen.on_create();
        screen.on_show();
    }
    // 挂载的 ViewGroup/TextView 全部随 Screen 释放，内核堆归还
    EXPECT_EQ(KernelHeap::instance().get_free_memory(), free_before);
}

// 脚本创建但未挂载（未被 create_ui 返回）的孤儿视图，由 lua_close 时的
// __gc 终结器回收，不得随 Screen 一起泄漏。
TEST_F(DynamicWatchFaceTest, OrphanLuaViewsReclaimedByGC) {
    const char* orphan_script = "function create_ui()\n"
                                "    local vg = aurora.ui.ViewGroup(0, 0, 192, 490)\n"
                                "    local ghost = aurora.ui.TextView(0, 0, \"never attached\", 65535)\n"
                                "    return vg\n"
                                "end\n";

    int fd = VfsManager::instance().open("/lfs/orphan_wf.lua", O_CREAT | O_WRONLY);
    if (fd >= 0) {
        VfsManager::instance().write(fd, orphan_script, strlen(orphan_script));
        VfsManager::instance().close(fd);
    }

    const size_t free_before = KernelHeap::instance().get_free_memory();
    {
        DynamicWatchFaceScreen screen("/lfs/orphan_wf.lua");
        screen.on_create();
    }
    // vg 随 Screen 释放；ghost (孤儿) 由 engine_ 析构时 lua_close 触发的
    // __gc 终结器 delete——若无终结器，此处内核堆将少归还一份 TextView。
    EXPECT_EQ(KernelHeap::instance().get_free_memory(), free_before);
}
