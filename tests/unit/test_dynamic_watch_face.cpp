#include <gtest/gtest.h>
#include "../../apps/watch/screens/dynamic_watch_face_screen.hpp"
#include "../../kernel/memory.hpp"
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
        const char* lua_script = 
            "function create_ui()\n"
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
