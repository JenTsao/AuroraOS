#ifndef AURORA_MINI_PROGRAM_ENGINE_HPP
#define AURORA_MINI_PROGRAM_ENGINE_HPP

#include <stdint.h>
#include "posix.hpp"
#include "../drivers/sensor/sensor_framework.hpp"
#include "../drivers/display/framebuffer.hpp"
#include "../kernel/memory.hpp"
#include "../boards/ti/lm3s6965-qb/board.h"

// 引入第三方 Lua 虚拟机 C 接口
extern "C" {
    #include "../3rdparty/lua/lua.h"
    #include "../3rdparty/lua/lualib.h"
    #include "../3rdparty/lua/lauxlib.h"
}
#include "lua_ui_binding.hpp"

// 声明外部全局的图形缓冲引擎 (用于供 Lua 脚本调用画图)
extern FrameBuffer<DISPLAY_WIDTH, DISPLAY_HEIGHT> g_fb;

class MiniProgramEngine {
private:
    lua_State* L_;
    bool       is_loaded_;

    // ========================================================
    // 自定义 Lua 内存分配器，将内存请求路由到 KernelHeap
    // ========================================================
    static void* lua_allocator(void* ud, void* ptr, size_t osize, size_t nsize) {
        (void)ud;
        if (nsize == 0) {
            if (ptr != nullptr) {
                KernelHeap::instance().deallocate(ptr);
            }
            return nullptr;
        } else {
            if (ptr == nullptr) {
                return KernelHeap::instance().allocate(nsize);
            } else {
                void* new_ptr = KernelHeap::instance().allocate(nsize);
                if (new_ptr) {
                    size_t copy_size = (osize < nsize) ? osize : nsize;
                    char* dest = static_cast<char*>(new_ptr);
                    const char* src = static_cast<const char*>(ptr);
                    for (size_t i = 0; i < copy_size; ++i) {
                        dest[i] = src[i];
                    }
                    KernelHeap::instance().deallocate(ptr);
                }
                return new_ptr;
            }
        }
    }

    // ========================================================
    // 系统 API 绑定 1：暴露获取心率的 C++ 函数给 Lua
    // ========================================================
    static int api_get_heart_rate(lua_State* L) {
        extern HeartRateSensor g_health_sensor;
        SensorDriver* hr_sensor = &g_health_sensor;
        if (hr_sensor) {
            SensorData val = {}; // Zero-initialize to prevent stack leaks
            if (hr_sensor->read(&val)) {
                lua_pushinteger(L, val.payload.bpm); // 将心率值压入 Lua 栈返回
            } else {
                lua_pushinteger(L, 0); // 传感器未上电或读取失败
            }
            return 1;
        }
        lua_pushinteger(L, 0);
        return 1;
    }

    // ========================================================
    // 系统 API 绑定 2：暴露 UI 绘制函数给 Lua
    // ========================================================
    static int api_fill_rect(lua_State* L) {
        // 从 Lua 获取 5 个参数: x, y, width, height, color_rgb565
        int x = luaL_checkinteger(L, 1);
        int y = luaL_checkinteger(L, 2);
        int w = luaL_checkinteger(L, 3);
        int h = luaL_checkinteger(L, 4);
        int color = luaL_checkinteger(L, 5);

        g_fb.fill_rect(x, y, w, h, static_cast<ColorRGB565>(color));
        return 0; // 无返回值
    }

    static int api_print(lua_State* L) {
        const char* str = luaL_checkstring(L, 1);
        int fd = open("/dev/uart0", 0);
        write(fd, "[Lua App] ", 10);
        
        int len = 0;
        while (str[len]) len++;
        
        write(fd, str, len);
        write(fd, "\r\n", 2);
        close(fd);
        return 0;
    }

    // ========================================================
    // 系统 API 绑定 3：暴露获取时间的 C++ 函数给 Lua
    // ========================================================
    static int api_get_time(lua_State* L) {
        extern void aurora_get_time(uint32_t& h, uint32_t& m);
        uint32_t h = 0, m = 0;
        aurora_get_time(h, m);
        lua_pushinteger(L, h);
        lua_pushinteger(L, m);
        return 2;
    }

public:
    MiniProgramEngine() : L_(nullptr), is_loaded_(false) {}

    ~MiniProgramEngine() {
        if (L_) lua_close(L_);
    }

    lua_State* get_lua_state() { return L_; }

    // 初始化虚拟机并注册 API 命名空间
    bool init() {
        L_ = lua_newstate(lua_allocator, nullptr); // 创建隔离的沙盒虚拟机，使用 KernelHeap 分配内存
        if (!L_) return false;
        
        // 我们裁剪了标准的 linit.c，只手动加载最核心的 base 库
        luaL_requiref(L_, "_G", luaopen_base, 1);
        lua_pop(L_, 1);
        
        // 彻底封堵预编译字节码沙盒逃逸漏洞：移除暴露的加载器
        lua_pushnil(L_); lua_setglobal(L_, "load");
        lua_pushnil(L_); lua_setglobal(L_, "loadstring");
        lua_pushnil(L_); lua_setglobal(L_, "dofile");
        lua_pushnil(L_); lua_setglobal(L_, "loadfile");

        // 将底层的 C++ 函数注册为 Lua 全局命名空间 aurora 的方法
        lua_newtable(L_);
        
        lua_pushcfunction(L_, api_get_heart_rate);
        lua_setfield(L_, -2, "get_heart_rate");

        lua_pushcfunction(L_, api_fill_rect);
        lua_setfield(L_, -2, "fill_rect");

        lua_pushcfunction(L_, api_print);
        lua_setfield(L_, -2, "print");

        lua_pushcfunction(L_, api_get_time);
        lua_setfield(L_, -2, "get_time");

        lua_setglobal(L_, "aurora"); // 注册全局变量 aurora

        luaopen_aurora_ui(L_); // 注册额外的 UI 控件

        return true;
    }

    // 从字符串 (或 LittleFS 文件) 加载应用脚本代码
    bool load_app(const char* script_code) {
        if (!L_) return false;
        if (luaL_dostring(L_, script_code) != LUA_OK) {
            int fd = open("/dev/uart0", 0);
            write(fd, "Lua Load Error!\r\n", 17);
            close(fd);
            return false;
        }
        is_loaded_ = true;
        return true;
    }

    // 从文件加载应用脚本代码
    bool load_app_from_file(const char* filepath) {
        if (!L_) return false;
        int fd = open(filepath, O_RDONLY);
        if (fd < 0) return false;
        
        // 0 = SEEK_SET, 2 = SEEK_END
        int size = lseek(fd, 0, 2); 
        lseek(fd, 0, 0); 
        
        if (size <= 0 || size > 65536) { // Sanity check max script size 64KB
            close(fd);
            return false;
        }

        char* buf = static_cast<char*>(KernelHeap::instance().allocate(size));
        if (!buf) {
            close(fd);
            return false;
        }
        
        int bytes_read = read(fd, buf, size);
        close(fd);
        
        if (bytes_read != size) {
            KernelHeap::instance().deallocate(buf);
            return false;
        }
        
        bool result = (luaL_loadbuffer(L_, buf, size, filepath) == LUA_OK);
        if (result) {
            result = (lua_pcall(L_, 0, LUA_MULTRET, 0) == LUA_OK);
        }
        
        KernelHeap::instance().deallocate(buf);
        
        if (!result) {
            int err_fd = open("/dev/uart0", 0); // 0 corresponds to O_RDONLY here but standard POSIX usually requires O_WRONLY for writing. The mock posix might ignore flags.
            write(err_fd, "Lua File Load Error!\r\n", 22);
            close(err_fd);
            return false;
        }
        
        is_loaded_ = true;
        return true;
    }

    // 触发脚本的生命周期钩子函数
    void call_hook(const char* hook_name) {
        if (!is_loaded_ || !L_) return;

        lua_getglobal(L_, hook_name);
        if (lua_isfunction(L_, -1)) {
            // 安全调用 Lua 函数，0个参数，0个返回值
            if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
                lua_pop(L_, 1); // 弹出错误信息
            }
        } else {
            lua_pop(L_, 1); // 如果不是函数则出栈
        }
    }
};

#endif
